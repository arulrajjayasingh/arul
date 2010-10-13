/* Copyright (c) 2010 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TransportManager.h"
#include "TransportFactory.h"

#include "TCPTransport.h"
#include "FastTransport.h"
#include "InfRCTransport.h"
#include "UdpDriver.h"

namespace RAMCloud {

static struct TcpTransportFactory : public TransportFactory {
    TcpTransportFactory()
        : TransportFactory("kernelTcp", "tcp") {}
    Transport* createTransport(const ServiceLocator* localServiceLocator) {
        return new TCPTransport(localServiceLocator);
    }
} tcpTransportFactory;

static struct FastUdpTransportFactory : public TransportFactory {
    FastUdpTransportFactory()
        : TransportFactory("fast+kernelUdp", "fast+udp") {}
    Transport* createTransport(const ServiceLocator* localServiceLocator) {
        return new FastTransport(new UdpDriver(localServiceLocator));
    }
} fastUdpTransportFactory;

static struct InfRCTransportFactory : public TransportFactory {
    InfRCTransportFactory()
        : TransportFactory("infinibandrc", "infrc") {}
    Transport* createTransport(const ServiceLocator* localServiceLocator) {
        return new InfRCTransport(localServiceLocator);
    }
} infRCTransportFactory;


/**
 * The single instance of #TransportManager.
 */
TransportManager transportManager;

TransportManager::TransportManager()
    : initialized(false)
    , transportFactories()
    , listening()
    , nextToListen(0)
    , transports()
{
    transportFactories.insert(&tcpTransportFactory);
    transportFactories.insert(&fastUdpTransportFactory);
    transportFactories.insert(&infRCTransportFactory);
}

TransportManager::~TransportManager()
{
    std::set<Transport*> toFree;
    BOOST_FOREACH(Transports::value_type protocolTransport, transports) {
        toFree.insert(protocolTransport.second);
    }
    BOOST_FOREACH(Transport* transport, toFree) {
        delete transport;
    }

}

/**
 * Construct the individual transports that will be used to send and receive.
 *
 * Calling this method is required before any calls to #serverRecv(), since the
 * receiving transports need to be instantiated with their local addresses
 * first. In this case, it must be called explicitly before any calls to
 * #getSession().
 *
 * Calling this method is not required if #serverRecv() will never be called.
 */
void
TransportManager::initialize(const char* localServiceLocator)
{
    assert(!initialized);

    std::vector<ServiceLocator> locators;
    ServiceLocator::parseServiceLocators(localServiceLocator, &locators);

    BOOST_FOREACH(TransportFactory* factory, transportFactories) {
        Transport* transport;
        BOOST_FOREACH(ServiceLocator& locator, locators) {
            if (factory->supports(locator.getProtocol().c_str())) {
                // The transport supports a protocol that we can receive
                // packets on.
                transport = factory->createTransport(&locator);
                listening.push_back(transport);
                goto insert_protocol_mappings;
            }
        }
        // The transport doesn't support any protocols that we can receive
        // packets on.
        transport = factory->createTransport(NULL);
 insert_protocol_mappings:
        BOOST_FOREACH(const char* protocol, factory->getProtocols()) {
            transports.insert(Transports::value_type(protocol, transport));
        }
    }
    initialized = true;
}

/**
 * Get a session on which to send RPC requests to a service.
 *
 * For now, multiple calls with the same argument will yield distinct sessions.
 * This will probably change later.
 *
 * \throw NoSuchKeyException
 *      A transport supporting one of the protocols claims a service locator
 *      option is missing.
 * \throw BadValueException
 *      A transport supporting one of the protocols claims a service locator
 *      option is malformed.
 * \throw TransportException
 *      No transport was found for this service locator.
 */
Transport::SessionRef
TransportManager::getSession(const char* serviceLocator)
{
    if (!initialized)
        initialize("");

    std::vector<ServiceLocator> locators;
    ServiceLocator::parseServiceLocators(serviceLocator, &locators);
    BOOST_FOREACH(ServiceLocator& locator, locators) {
        BOOST_FOREACH(Transports::value_type protocolTransport,
                      transports.equal_range(locator.getProtocol())) {
            Transport* transport = protocolTransport.second;
            try {
                return transport->getSession(locator);
            } catch (TransportException& e) {
                // TODO(ongaro): Transport::getName() would be nice here.
                LOG(DEBUG, "Transport %p refused to open session for %s",
                    transport, locator.getOriginalString().c_str());
            }
        }
    }
    throw TransportException();
}

/**
 * Receive an RPC request. This will block until receiving a packet from any
 * listening transport.
 * \throw UnrecoverableTransportException
 *      There are no listening transports, so this call would block forever.
 */
Transport::ServerRpc*
TransportManager::serverRecv()
{
    if (!initialized || listening.empty())
        throw UnrecoverableTransportException("no transports to listen on");
    while (true) {
        if (nextToListen >= listening.size())
            nextToListen = 0;
        Transport* transport = listening[nextToListen++];

        Transport::ServerRpc* rpc = transport->serverRecv();
        if (rpc != NULL)
            return rpc;
    }
}

} // namespace RAMCloud