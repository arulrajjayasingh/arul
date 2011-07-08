/* Copyright (c) 2010-2011 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef RAMCLOUD_TCPTRANSPORT_H
#define RAMCLOUD_TCPTRANSPORT_H

#include <event.h>
#include <queue>

#include "BoostIntrusive.h"
#include "Dispatch.h"
#include "IpAddress.h"
#include "Tub.h"
#include "Syscall.h"
#include "Transport.h"

namespace RAMCloud {

/**
 * A simple transport mechanism based on TCP/IP provided by the kernel.
 * This implementation is unlikely to be fast enough for production use;
 * this class will be used primarily for development and as a baseline
 * for testing.  The goal is to provide an implementation that is about as
 * fast as possible, given its use of kernel-based TCP/IP.
 */
class TcpTransport : public Transport {
  public:
    explicit TcpTransport(const ServiceLocator* serviceLocator = NULL);
    ~TcpTransport();
    SessionRef getSession(const ServiceLocator& serviceLocator) {
        return new TcpSession(serviceLocator);
    }
    string getServiceLocator() {
        return locatorString;
    }
    void registerMemory(void* base, size_t bytes) {}

    class TcpServerRpc;
  PRIVATE:
    class ServerSocketHandler;
    class IncomingMessage;
    class ClientSocketHandler;
    class Socket;
    class TcpSession;
    friend class TcpTransportTest;
    friend class AcceptHandler;
    friend class ServerSocketHandler;
    /**
     * Header for request and response messages: precedes the actual data
     * of the message in all transmissions.
     */
    struct Header {
        /// Unique identifier for this RPC: generated on the client, and
        /// returned by the server in responses.  This field makes it
        /// possible for a client to have multiple outstanding RPCs to
        /// the same server.
        uint64_t nonce;

        /// The size in bytes of the payload (which follows immediately).
        /// Must be less than or equal to #MAX_RPC_LEN.
        uint32_t len;
    } __attribute__((packed));

    /**
     * Used to manage the receipt of a message (on either client or server)
     * using an event-based approach.
     */
    class IncomingMessage {
      friend class ServerSocketHandler;
      friend class TcpTransportTest;
      friend class TcpServerRpc;
      public:
        IncomingMessage(Buffer* buffer, TcpSession* session);
        bool readMessage(int fd);
      PRIVATE:
        Header header;

        /// The number of bytes of header that have been successfully
        /// received so far; 0 means the header has not yet been received;
        /// sizeof(Header) means the header is complete.
        uint32_t headerBytesReceived;

        /// Counts the number of bytes in the message body that have been
        /// received so far.
        uint32_t messageBytesReceived;

        /// The number of bytes of input message that we will actually retain
        /// (normally this is the same as header.len, but it may be less
        /// if header.len is illegally large or if the entire message is being
        /// discarded).
        uint32_t messageLength;

        /// Buffer in which incoming message will be stored (not including
        /// transport-specific header).  NULL means the message will be
        /// discarded.
        Buffer *buffer;

        /// Session that will find the buffer to use for this message once
        /// the header has arrived (or NULL).
        TcpSession* session;

        DISALLOW_COPY_AND_ASSIGN(IncomingMessage);
    };

  public:
    /**
     * The TCP implementation of Transport::ServerRpc.
     */
    class TcpServerRpc : public Transport::ServerRpc {
      friend class ServerSocketHandler;
      friend class TcpTransportTest;
      friend class TcpTransport;
      public:
        virtual ~TcpServerRpc()
        {
            TEST_LOG("deleted");
        }
        void sendReply();
      PRIVATE:
        TcpServerRpc(Socket* socket, int fd)
            : fd(fd), socket(socket), message(&requestPayload, NULL),
            queueEntries() { }

        int fd;                   /// File descriptor of the socket on
                                  /// which the request was received.
        Socket* socket;           /// Transport state corresponding to fd.
        IncomingMessage message;  /// Records state of partially-received
                                  /// request.
        IntrusiveListHook queueEntries;
                                  /// Used to link this RPC onto the
                                  /// rpcsWaitingToReply list of the Socket.

        DISALLOW_COPY_AND_ASSIGN(TcpServerRpc);
    };

    /**
     * The TCP implementation of Transport::ClientRpc.
     */
    class TcpClientRpc : public Transport::ClientRpc {
      public:
        friend class TcpTransportTest;
        friend class TcpTransport;
        friend class TcpSession;
        explicit TcpClientRpc(TcpSession* session, Buffer*request,
                Buffer* reply, uint64_t nonce)
            : request(request), reply(reply), nonce(nonce),
              session(session), sent(false), queueEntries()
               { }
      PROTECTED:
        virtual void cancelCleanup();
      PRIVATE:
        Buffer* request;          /// Contains request message.
        Buffer* reply;            /// Client's buffer for response.
        uint64_t nonce;           /// Unique identifier for this RPC; used
                                  /// to pair the RPC with its response.
        TcpSession *session;      /// Session used for this RPC.
        bool sent;                /// True means the request has been sent
                                  /// and we are waiting for the response;
                                  /// false means this RPC is queued on
                                  /// rpcsWaitingToSend.
        IntrusiveListHook queueEntries;
                                  /// Used to link this RPC onto the
                                  /// rpcsWaitingToSend and
                                  /// rpcsWaitingForResponse lists of session.
        DISALLOW_COPY_AND_ASSIGN(TcpClientRpc);
    };

  PRIVATE:
    void closeSocket(int fd);
    static ssize_t recvCarefully(int fd, void* buffer, size_t length);
    static int sendMessage
        (int fd, uint64_t nonce, Buffer& payload,
            int bytesToSend);

    /**
     * An exception that is thrown when a socket has been closed by the peer.
     */
    class TcpTransportEof : public TransportException {
      public:
        explicit TcpTransportEof(const CodeLocation& where)
            : TransportException(where) {}
    };

    /**
     * An event handler that will accept connections on a socket.
     */
    class AcceptHandler : public Dispatch::File {
      public:
        AcceptHandler(int fd, TcpTransport* transport);
        virtual void handleFileEvent(int events);
      PRIVATE:
        // Transport that manages this socket.
        TcpTransport* transport;
        DISALLOW_COPY_AND_ASSIGN(AcceptHandler);
    };

    /**
     * An event handler that moves bytes to and from a server's socket.
     */
    class ServerSocketHandler : public Dispatch::File {
      public:
        ServerSocketHandler(int fd, TcpTransport* transport, Socket* socket);
        virtual void handleFileEvent(int events);
      PRIVATE:
        // The following variables are just copies of constructor arguments.
        int fd;
        TcpTransport* transport;
        Socket* socket;
        DISALLOW_COPY_AND_ASSIGN(ServerSocketHandler);
    };

    /**
     * An event handler that moves bytes to and from a client-side sockes.
     */
    class ClientSocketHandler : public Dispatch::File {
      public:
        ClientSocketHandler(int fd, TcpSession* session);
        virtual void handleFileEvent(int events);
      PRIVATE:
        // The following variables are just copies of constructor arguments.
        int fd;
        TcpSession* session;
        DISALLOW_COPY_AND_ASSIGN(ClientSocketHandler);
    };

    /**
     * The TCP implementation of Sessions (stored on a client to manage its
     * interactions with a particular server).
     */
    class TcpSession : public Session {
      friend class ClientIncomingMessage;
      friend class TcpTransportTest;
      friend class TcpClientRpc;
      friend class ClientSocketHandler;
      public:
        explicit TcpSession(const ServiceLocator& serviceLocator);
        ~TcpSession();
        ClientRpc* clientSend(Buffer* request, Buffer* reply)
            __attribute__((warn_unused_result));
        Buffer* findRpc(Header& header);
        void release() {
            delete this;
        }
      PRIVATE:
#if TESTING
        TcpSession() : address(), fd(-1), serial(1), rpcsWaitingToSend(),
            bytesLeftToSend(0), rpcsWaitingForResponse(), current(NULL),
            message(), clientIoHandler(), errorInfo() { }
#endif
        void close();
        static void tryReadReply(int fd, int16_t event, void *arg);

        IpAddress address;        /// Server to which requests will be sent.
        int fd;                   /// File descriptor for the socket that
                                  /// connects to address  -1 means no socket
                                  /// open.
        uint64_t serial;          /// Used to generate nonces for RPCs: starts
                                  /// at 1 and increments for each RPC.

        INTRUSIVE_LIST_TYPEDEF(TcpClientRpc, queueEntries) ClientRpcList;
        ClientRpcList rpcsWaitingToSend;
                                  /// RPCs whose request messages have not yet
                                  /// been transmitted.  The front RPC on this
                                  /// list is currently being transmitted.
        int bytesLeftToSend;      /// The number of (trailing) bytes in the
                                  /// first RPC on rpcsWaitingToSend that still
                                  /// need to be transmitted, once fd becomes
                                  /// writable again.  -1 or 0 means there
                                  /// are no RPCs waiting to be transmitted.
        ClientRpcList rpcsWaitingForResponse;
                                  /// RPCs whose request messages have been
                                  /// transmitted, but whose responses have
                                  /// not yet been received.
        TcpClientRpc* current;    /// RPC for which we are currently receiving
                                  /// a response (NULL if none).
        Tub<IncomingMessage> message;
                                  /// Records state of partially-received
                                  /// reply for current.
        Tub<ClientSocketHandler> clientIoHandler;
                                  /// Used to get notified when response data
                                  /// arrives.
        string errorInfo;         /// If the session is no longer usable,
                                  /// this variable indicates why.
        DISALLOW_COPY_AND_ASSIGN(TcpSession);
    };

    static Syscall* sys;

    /// Service locator used to open server socket (empty string if this
    /// isn't a server). May differ from what was passed to the constructor
    /// if dynamic ports are used.
    string locatorString;

    /// File descriptor used by servers to listen for connections from
    /// clients.  -1 means this instance is not a server.
    int listenSocket;

    /// Used to wait for listenSocket to become readable.
    Tub<AcceptHandler> acceptHandler;

    /// Used to hold information about a file descriptor associated with
    /// a socket, on which RPC requests may arrive.
    class Socket {
        public:
        Socket(int fd, TcpTransport *transport)
                : rpc(NULL), ioHandler(fd, transport, this),
                rpcsWaitingToReply(), bytesLeftToSend(0) { }
        ~Socket();
        TcpServerRpc *rpc;        /// Incoming RPC that is in progress for
                                  /// this fd, or NULL if none.
        ServerSocketHandler ioHandler;
                                  /// Used to get notified whenever data
                                  /// arrives on this fd.
        INTRUSIVE_LIST_TYPEDEF(TcpServerRpc, queueEntries) ServerRpcList;
        ServerRpcList rpcsWaitingToReply;
                                  /// RPCs whose response messages have not yet
                                  /// been transmitted.  The front RPC on this
                                  /// list is currently being transmitted.
        int bytesLeftToSend;      /// The number of (trailing) bytes in the
                                  /// front RPC on rpcsWaitingToReply that still
                                  /// need to be transmitted, once fd becomes
                                  /// writable again.  -1 or 0 means there are
                                  /// no RPCs waiting.
        DISALLOW_COPY_AND_ASSIGN(Socket);
    };

    /// Keeps track of all of our open client connections. Entry i has
    /// information about file descriptor i (NULL means no client
    /// is currently connected).
    std::vector<Socket*> sockets;

    /// Counts the number of nonzero-size partial messages sent by
    /// sendMessage (for testing only).
    static int messageChunks;

    DISALLOW_COPY_AND_ASSIGN(TcpTransport);
};

}  // namespace RAMCloud

#endif  // RAMCLOUD_TCPTRANSPORT_H
