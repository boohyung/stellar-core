// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "overlay/TCPPeer.h"
#include "database/Database.h"
#include "main/Application.h"
#include "main/Config.h"
#include "main/ErrorMessages.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "overlay/LoadManager.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/PeerManager.h"
#include "overlay/StellarXDR.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdrpp/marshal.h"

using namespace soci;

namespace stellar
{

using namespace std;

///////////////////////////////////////////////////////////////////////
// TCPPeer
///////////////////////////////////////////////////////////////////////

const size_t TCPPeer::BUFSZ;

TCPPeer::TCPPeer(Application& app, Peer::PeerRole role,
                 std::shared_ptr<TCPPeer::SocketType> socket)
    : Peer(app, role), mSocket(socket)
{
}

TCPPeer::pointer
TCPPeer::initiate(Application& app, PeerBareAddress const& address)
{
    assert(address.getType() == PeerBareAddress::Type::IPv4);

    CLOG(DEBUG, "Overlay") << "TCPPeer:initiate"
                           << " to " << address.toString();
    assertThreadIsMain();
    auto socket =
        make_shared<SocketType>(app.getClock().getIOContext(), BUFSZ, BUFSZ);
    auto result = make_shared<TCPPeer>(app, WE_CALLED_REMOTE, socket);
    result->mAddress = address;
    result->startIdleTimer();
    asio::ip::tcp::endpoint endpoint(
        asio::ip::address::from_string(address.getIP()), address.getPort());
    socket->next_layer().async_connect(
        endpoint, [result](asio::error_code const& error) {
            asio::error_code ec;
            if (!error)
            {
                asio::ip::tcp::no_delay nodelay(true);
                result->mSocket->next_layer().set_option(nodelay, ec);
            }
            else
            {
                ec = error;
            }

            result->connectHandler(ec);
        });
    return result;
}

TCPPeer::pointer
TCPPeer::accept(Application& app, shared_ptr<TCPPeer::SocketType> socket)
{
    assertThreadIsMain();
    shared_ptr<TCPPeer> result;
    asio::error_code ec;

    asio::ip::tcp::no_delay nodelay(true);
    socket->next_layer().set_option(nodelay, ec);

    if (!ec)
    {
        CLOG(DEBUG, "Overlay") << "TCPPeer:accept"
                               << "@" << app.getConfig().PEER_PORT;
        result = make_shared<TCPPeer>(app, REMOTE_CALLED_US, socket);
        result->startIdleTimer();
        result->startRead();
    }
    else
    {
        CLOG(DEBUG, "Overlay")
            << "TCPPeer:accept"
            << "@" << app.getConfig().PEER_PORT << " error " << ec.message();
    }

    return result;
}

TCPPeer::~TCPPeer()
{
    assertThreadIsMain();
    mIdleTimer.cancel();
    if (mSocket)
    {
        // Ignore: this indicates an attempt to cancel events
        // on a not-established socket.
        asio::error_code ec;

#ifndef _WIN32
        // This always fails on windows and ASIO won't
        // even build it.
        mSocket->next_layer().cancel(ec);
#endif
        mSocket->close(ec);
    }
}

std::string
TCPPeer::getIP() const
{
    std::string result;

    asio::error_code ec;
    auto ep = mSocket->next_layer().remote_endpoint(ec);
    if (!ec)
    {
        result = ep.address().to_string();
    }

    return result;
}

void
TCPPeer::sendMessage(xdr::msg_ptr&& xdrBytes)
{
    if (mState == CLOSING)
    {
        CLOG(ERROR, "Overlay")
            << "Trying to send message to " << toString() << " after drop";
        CLOG(ERROR, "Overlay") << REPORT_INTERNAL_BUG;
        return;
    }

    if (Logging::logTrace("Overlay"))
        CLOG(TRACE, "Overlay") << "TCPPeer:sendMessage to " << toString();
    assertThreadIsMain();

    // places the buffer to write into the write queue
    TimestampedMessage msg;
    msg.mEnqueuedTime = mApp.getClock().now();
    msg.mMessage = std::move(xdrBytes);
    auto tsm = std::make_shared<TimestampedMessage>(std::move(msg));

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    self->mWriteQueue.emplace(tsm);

    if (!self->mWriting)
    {
        self->mWriting = true;
        // kick off the async write chain if we're the first one
        self->messageSender();
    }
}

void
TCPPeer::shutdown()
{
    if (mShutdownScheduled)
    {
        // should not happen, leave here for debugging purposes
        CLOG(ERROR, "Overlay") << "Double schedule of shutdown " << toString();
        CLOG(ERROR, "Overlay") << REPORT_INTERNAL_BUG;
        return;
    }

    mIdleTimer.cancel();
    mShutdownScheduled = true;
    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    // To shutdown, we first queue up our desire to shutdown in the strand,
    // behind any pending read/write calls. We'll let them issue first.
    self->getApp().postOnMainThread(
        [self]() {
            // Gracefully shut down connection: this pushes a FIN packet into
            // TCP which, if we wanted to be really polite about, we would wait
            // for an ACK from by doing repeated reads until we get a 0-read.
            //
            // But since we _might_ be dropping a hostile or unresponsive
            // connection, we're going to just post a close() immediately after,
            // and hope the kernel does something useful as far as putting any
            // queued last-gasp ERROR_MSG packet on the wire.
            //
            // All of this is voluntary. We can also just close(2) here and be
            // done with it, but we want to give some chance of telling peers
            // why we're disconnecting them.
            asio::error_code ec;
            self->mSocket->next_layer().shutdown(
                asio::ip::tcp::socket::shutdown_both, ec);
            if (ec)
            {
                CLOG(DEBUG, "Overlay")
                    << "TCPPeer::drop shutdown socket failed: " << ec.message();
            }
            self->getApp().postOnMainThread(
                [self]() {
                    // Close fd associated with socket. Socket is already shut
                    // down, but depending on platform (and apparently whether
                    // there was unread data when we issued shutdown()) this
                    // call might push RST onto the wire, or some other action;
                    // in any case it has to be done to free the OS resources.
                    //
                    // It will also, at this point, cancel any pending asio
                    // read/write handlers, i.e. fire them with an error code
                    // indicating cancellation.
                    asio::error_code ec2;
                    self->mSocket->close(ec2);
                    if (ec2)
                    {
                        CLOG(DEBUG, "Overlay")
                            << "TCPPeer::drop close socket failed: "
                            << ec2.message();
                    }
                },
                "TCPPeer: close");
        },
        "TCPPeer: shutdown");
}

void
TCPPeer::messageSender()
{
    assertThreadIsMain();

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    // if nothing to do, flush and return
    if (mWriteQueue.empty())
    {
        mLastEmpty = mApp.getClock().now();
        mSocket->async_flush([self](asio::error_code const& ec, std::size_t) {
            self->writeHandler(ec, 0);
            if (!ec)
            {
                if (!self->mWriteQueue.empty())
                {
                    self->messageSender();
                }
                else
                {
                    self->mWriting = false;
                    // there is nothing to send and delayed shutdown was
                    // requested - time to perform it
                    if (self->mDelayedShutdown)
                    {
                        self->shutdown();
                    }
                }
            }
        });
        return;
    }

    // peek the buffer from the queue
    // do not remove it yet as we need the buffer for the duration of the
    // write operation
    auto tsm = mWriteQueue.front();
    tsm->mIssuedTime = mApp.getClock().now();

    asio::async_write(
        *(mSocket.get()),
        asio::buffer(tsm->mMessage->raw_data(), tsm->mMessage->raw_size()),
        [self, tsm](asio::error_code const& ec, std::size_t length) {
            self->writeHandler(ec, length);
            tsm->mCompletedTime = self->mApp.getClock().now();
            tsm->recordWriteTiming(self->getOverlayMetrics());
            self->mWriteQueue.pop(); // done with front element

            // continue processing the queue/flush
            if (!ec)
            {
                self->messageSender();
            }
        });
}

void
TCPPeer::TimestampedMessage::recordWriteTiming(OverlayMetrics& metrics)
{
    auto qdelay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        mIssuedTime - mEnqueuedTime);
    auto wdelay = std::chrono::duration_cast<std::chrono::nanoseconds>(
        mCompletedTime - mIssuedTime);
    metrics.mMessageDelayInWriteQueueTimer.Update(qdelay);
    metrics.mMessageDelayInAsyncWriteTimer.Update(wdelay);
}

void
TCPPeer::writeHandler(asio::error_code const& error,
                      std::size_t bytes_transferred)
{
    assertThreadIsMain();
    mLastWrite = mApp.getClock().now();

    if (error)
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            getOverlayMetrics().mErrorWrite.Mark();
            CLOG(ERROR, "Overlay")
                << "Error during sending message to " << toString();
        }
        if (mDelayedShutdown)
        {
            // delayed shutdown was requested - time to perform it
            shutdown();
        }
        else
        {
            // no delayed shutdown - we can drop normally
            drop("error during write", Peer::DropDirection::WE_DROPPED_REMOTE,
                 Peer::DropMode::IGNORE_WRITE_QUEUE);
        }
    }
    else if (bytes_transferred != 0)
    {
        LoadManager::PeerContext loadCtx(mApp, mPeerID);
        getOverlayMetrics().mMessageWrite.Mark();
        getOverlayMetrics().mByteWrite.Mark(bytes_transferred);

        ++mPeerMetrics.mMessageWrite;
        mPeerMetrics.mByteWrite += bytes_transferred;
    }
}

void
TCPPeer::startRead()
{
    assertThreadIsMain();
    if (shouldAbort())
    {
        return;
    }

    const size_t HDRSZ = 4;
    auto self = static_pointer_cast<TCPPeer>(shared_from_this());

    assert(self->mIncomingHeader.size() == 0);

    if (Logging::logTrace("Overlay"))
        CLOG(TRACE, "Overlay") << "TCPPeer::startRead to " << self->toString();

    self->mIncomingHeader.resize(HDRSZ);

    // We read large-ish (1MB) buffers of data from TCP which might have quite a
    // few messages in them. We want to digest as many of these _synchronously_
    // as we can before we issue an async_read against ASIO.
    YieldTimer yt(mApp.getClock());
    while (mSocket->in_avail() >= HDRSZ && yt.shouldKeepGoing())
    {
        size_t n = mSocket->read_some(asio::buffer(mIncomingHeader));
        if (n != HDRSZ)
        {
            drop("error during header read_some",
                 Peer::DropDirection::WE_DROPPED_REMOTE,
                 Peer::DropMode::IGNORE_WRITE_QUEUE);
            return;
        }
        size_t length = static_cast<size_t>(getIncomingMsgLength());
        if (length != 0)
        {
            if (mSocket->in_avail() >= length)
            {
                // We can finish reading a full message here synchronously.
                mIncomingBody.resize(length);
                n = mSocket->read_some(asio::buffer(mIncomingBody));
                if (n != length)
                {
                    drop("error during body read_some",
                         Peer::DropDirection::WE_DROPPED_REMOTE,
                         Peer::DropMode::IGNORE_WRITE_QUEUE);
                    return;
                }
                receivedBytes(length, true);
                recvMessage();
            }
            else
            {
                // We read a header synchronously, but don't have enough data in
                // the buffered_stream to read the body synchronously. Pretend
                // we just finished reading the header asynchronously, and punt
                // to readHeaderHandler to let it re-read the header and issue
                // an async read for the body.
                readHeaderHandler(asio::error_code(), HDRSZ);
                return;
            }
        }
    }

    // If there wasn't enough readable in the buffered stream to even get a
    // header (message length), issue an async_read and hope that the buffering
    // pulls in much more than just the 4 bytes we ask for here.
    getOverlayMetrics().mAsyncRead.Mark();
    asio::async_read(*(self->mSocket.get()),
                     asio::buffer(self->mIncomingHeader),
                     [self](asio::error_code ec, std::size_t length) {
                         if (Logging::logTrace("Overlay"))
                             CLOG(TRACE, "Overlay")
                                 << "TCPPeer::startRead calledback " << ec
                                 << " length:" << length;
                         self->readHeaderHandler(ec, length);
                     });
}

int
TCPPeer::getIncomingMsgLength()
{
    int length = mIncomingHeader[0];
    length &= 0x7f; // clear the XDR 'continuation' bit
    length <<= 8;
    length |= mIncomingHeader[1];
    length <<= 8;
    length |= mIncomingHeader[2];
    length <<= 8;
    length |= mIncomingHeader[3];
    if (length <= 0 ||
        (!isAuthenticated() && (length > MAX_UNAUTH_MESSAGE_SIZE)) ||
        length > MAX_MESSAGE_SIZE)
    {
        getOverlayMetrics().mErrorRead.Mark();
        CLOG(ERROR, "Overlay")
            << "TCP: message size unacceptable: " << length
            << (isAuthenticated() ? "" : " while not authenticated");
        drop("error during read", Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
        length = 0;
    }
    return (length);
}

void
TCPPeer::connected()
{
    startRead();
}

void
TCPPeer::readHeaderHandler(asio::error_code const& error,
                           std::size_t bytes_transferred)
{
    assertThreadIsMain();
    // LOG(DEBUG) << "TCPPeer::readHeaderHandler "
    //     << "@" << mApp.getConfig().PEER_PORT
    //     << " to " << mRemoteListeningPort
    //     << (error ? "error " : "") << " bytes:" << bytes_transferred;

    if (!error)
    {
        receivedBytes(bytes_transferred, false);
        int length = getIncomingMsgLength();
        if (length != 0)
        {
            mIncomingBody.resize(length);
            auto self = static_pointer_cast<TCPPeer>(shared_from_this());
            asio::async_read(*mSocket.get(), asio::buffer(mIncomingBody),
                             [self](asio::error_code ec, std::size_t length) {
                                 self->readBodyHandler(ec, length);
                             });
        }
    }
    else
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            getOverlayMetrics().mErrorRead.Mark();
            CLOG(DEBUG, "Overlay")
                << "readHeaderHandler error: " << error.message() << ": "
                << toString();
        }
        drop("error during read", Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
}

void
TCPPeer::readBodyHandler(asio::error_code const& error,
                         std::size_t bytes_transferred)
{
    assertThreadIsMain();
    // LOG(DEBUG) << "TCPPeer::readBodyHandler "
    //     << "@" << mApp.getConfig().PEER_PORT
    //     << " to " << mRemoteListeningPort
    //     << (error ? "error " : "") << " bytes:" << bytes_transferred;

    if (!error)
    {
        receivedBytes(bytes_transferred, true);
        recvMessage();
        mIncomingHeader.clear();
        startRead();
    }
    else
    {
        if (isConnected())
        {
            // Only emit a warning if we have an error while connected;
            // errors during shutdown or connection are common/expected.
            getOverlayMetrics().mErrorRead.Mark();
            CLOG(ERROR, "Overlay")
                << "readBodyHandler error: " << error.message() << " :"
                << toString();
        }
        drop("error during read", Peer::DropDirection::WE_DROPPED_REMOTE,
             Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
}

void
TCPPeer::recvMessage()
{
    assertThreadIsMain();
    try
    {
        xdr::xdr_get g(mIncomingBody.data(),
                       mIncomingBody.data() + mIncomingBody.size());
        AuthenticatedMessage am;
        xdr::xdr_argpack_archive(g, am);
        Peer::recvMessage(am);
    }
    catch (xdr::xdr_runtime_error& e)
    {
        CLOG(ERROR, "Overlay") << "recvMessage got a corrupt xdr: " << e.what();
        sendErrorAndDrop(ERR_DATA, "received corrupt XDR",
                         Peer::DropMode::IGNORE_WRITE_QUEUE);
    }
}

void
TCPPeer::drop(std::string const& reason, DropDirection dropDirection,
              DropMode dropMode)
{
    assertThreadIsMain();
    if (shouldAbort())
    {
        return;
    }

    if (mState != GOT_AUTH)
    {
        CLOG(DEBUG, "Overlay") << "TCPPeer::drop " << toString() << " in state "
                               << mState << " we called:" << mRole;
    }
    else if (dropDirection == Peer::DropDirection::WE_DROPPED_REMOTE)
    {
        CLOG(INFO, "Overlay")
            << "Dropping peer " << toString() << "; reason: " << reason;
    }
    else
    {
        CLOG(INFO, "Overlay")
            << "Peer " << toString() << " dropped us; reason: " << reason;
    }

    mState = CLOSING;

    auto self = static_pointer_cast<TCPPeer>(shared_from_this());
    getApp().getOverlayManager().removePeer(this);

    // if write queue is not empty, messageSender will take care of shutdown
    if ((dropMode == Peer::DropMode::IGNORE_WRITE_QUEUE) || !mWriting)
    {
        self->shutdown();
    }
    else
    {
        self->mDelayedShutdown = true;
    }
}
}
