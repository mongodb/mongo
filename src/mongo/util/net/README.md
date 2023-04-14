# TLS

## Table Of Contents

- [High Level Overview](#high-level-overview)
    - [ASIO](#asio)
    - [FIPS Mode](#fips-mode)
    - [Authentication & Authorization](#authentication--authorization)
    - [The Transport Layer](#the-transport-layer)
    - [SNI](#sni)
- [Protocol Theory](#protocol-theory)
    - [The TLS Handshake](#the-tls-handshake)
    - [Ciphers](#ciphers)
- [X.509](#x509)
    - [Certificate Authorities](#certificate-authorities)
    - [Certificate Metadata](#certificate-metadata)
    - [Certificate Expiration and Revocation](#certificate-expiration-and-revocation)
    - [Member Certificates](#member-certificates)

## High Level Overview

TLS stands for **Transport Layer Security**. It used to be known as **SSL (Secure Sockets Layer)**, but was renamed to
TLS in 1999. TLS is a specification for the secure transport of data over a network connection using both 
[**symmetric**](https://en.wikipedia.org/wiki/Symmetric-key_algorithm) and 
[**asymmetric**](https://en.wikipedia.org/wiki/Public-key_cryptography) key cryptography, most commonly with certificates.
There is no "official" implementation of TLS, not even from the [RFC](https://tools.ietf.org/html/rfc5246) authors. 
There are, however, several different implementations commonly used in different environments. MongoDB uses three different 
implementations of TLS:

1. [OpenSSL](https://www.openssl.org/docs/) on _Linux_
    * OpenSSL is also available on MacOS and Windows, but we do not officially support those configurations anymore.
2. [SChannel](https://docs.microsoft.com/en-us/windows-server/security/tls/tls-ssl-schannel-ssp-overview) which is made
by Microsoft and is avialable exclusively on _Windows_.
3. [Secure Transport](https://developer.apple.com/documentation/security/secure_transport) which is made by Apple and is
available exclusively on _MacOS_.

We manage TLS through an interface called 
[`SSLManagerInterface`](https://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.h#L181). There are 
three different implementations of this interface for each implementation of TLS respectively:

1. [`SSLManagerOpenSSL`](ssl_manager_openssl.cpp)
2. [`SSLManagerWindows`](ssl_manager_windows.cpp)
3. [`SSLManagerApple`](ssl_manager_apple.cpp)

Every SSLManager has a set of key methods that describe the general idea of establishing a connection over TLS:

* [`connect`](https://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.h#L193): initiates a TLS connection
* [`accept`](https://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.h#L201): waits for a peer to 
initiate a TLS connection
* [`parseAndValidatePeerCertificate`](https://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.h#L268): 
parses a certificate acquired from a peer during connection negotiation and validates it.

The SSLManagers are wrappers around these TLS implementations such that they conform to our practices and standards, and
so that all interaction with them can be done through a standard interface. Note that `connect` and `accept` are
are only for the _synchronous_ code paths used by the transport layer. For the _asynchronous_ paths used by the 
transport layer, we use **ASIO**.

Also note that we used to do synchronous networking through the [`mongo::Socket`](sock.h) codepath; however, this has
been deprecated and will eventually be removed.

### ASIO

We use a third-party asynchronous networking library called [**ASIO**](https://think-async.com/Asio/) to accept and 
establish connections, as well as read and write wire protocol messages. ASIO's 
[`GenericSocket`](https://www.boost.org/doc/libs/1_66_0/doc/html/boost_asio/reference/generic__stream_protocol/socket.html) object is used to 
read and write from sockets. We do this to handle an interesting quirk about MongoDB's use of TLS. Typically, services
will listen for clear text connections on one port, and TLS connections on another port. MongoDB, however, only listens
on one port. Since we give administrators the ability to upgrade clusters to TLS, we need to use the same port to avoid
having to edit the replset config. Because the transport layer has to be ready to receive TLS or cleartext traffic, it
has to be able to determine what it is speaking on the fly by inspecting the first message it receives from an incoming
connection to determine if it needs to speak cleartext or TLS. If the transport layer believes it is speaking TLS, 
[it will create a new `asio::ssl::stream<GenericSocket>` object which wraps the underlying physical connection's `GenericSocket` object](../../transport/asio_session.h).
The outer `Socket` accepts requests to perform reads and writes, but encrypts/decrypts data before/after interacting 
with the physical socket. ASIO also provides the 
[implementation](https://github.com/mongodb/mongo/blob/master/src/mongo/transport/asio_session.h#L75)
for the TLS socket for OpenSSL, but we provide [our own implementation](ssl) for SChannel and Secure Transport.

### FIPS Mode

[`tlsFIPSMode`](https://docs.mongodb.com/manual/reference/program/mongod/#cmdoption-mongod-tlsfipsmode) 
is a configuration option that allows TLS to operate in  [**FIPS** mode](https://www.openssl.org/docs/fips.html), 
meaning that all cryptography must be done on a FIPS-140 certified device (physical or virtual). As such, `tlsFIPSMode` 
can only be enabled if a FIPS-compliant library is installed.

### Authentication & Authorization

MongoDB can use the X.509 certificates presented by clients during the TLS handshake for 
[**Authentication**](https://docs.mongodb.com/manual/tutorial/configure-x509-client-authentication/) and **Authorization**, 
although X.509 authorization is uncommon. X.509 authorization can be done by embedding roles 
into certificates along with the public key and certificate metadata. Internal authorization, using privilege documents 
stored in the database, is more commonly used for acquiring access rights, and X.509 authorization is not a typical 
use case of certificates in general. We do not provide any utility for generating these unique certificates, but the 
logic for parsing them is in 
[`parsePeerRoles`](https://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.h#L294).

X.509 certificates _are_ commonly used for _authentication_. 
[`SSLManager::parseAndValidatePeerCertificate`](https://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.h#L268)
extracts information required for authentication, such as the client name, during connection negotiation. We use 
TLS for authentication, as being able to successfully perform a cryptographic handshake after key exchange should be as 
sufficient proof of authenticity as providing a username and password. This idea is the same as using one's RSA public 
key to authenticate to an SSH server instead of having to type a password. When a server is configured to authenticate 
with TLS (using the 
[**MONGODB-X509**](https://docs.mongodb.com/manual/reference/program/mongo/#cmdoption-mongo-authenticationmechanism) 
mechanism), it receives a certificate from the client during the initial TLS handshake, extracts the subject name, which
MongoDB will consider a username, and performs the cryptographic handshake. If the handshake succeeds, then the server 
will note that the client proved ownership of the presented certificate. The authentication logic happens in the 
[`authX509`](https://github.com/mongodb/mongo/blob/master/src/mongo/client/authenticate.cpp#L127) function, although
there are many callers of this function that use it in different ways. Later, when that client tries to authenticate, 
the server will know that the previous TLS handshake has proved their authenticity, and will grant themm the appropriate
access rights.

### The Transport Layer

The _transport layer_ calls appropriate TLS functions inside of its own related functions. 
[`TransportLayerManager::connect`](https://github.com/mongodb/mongo/blob/master/src/mongo/transport/transport_layer_manager.h#L66) 
will make a call to 
[`AsioSession::handshakeSSLForEgress`](https://github.com/mongodb/mongo/blob/master/src/mongo/transport/asio_transport_layer.cpp#L496)
when it needs to speak TLS. This works the same for 
[`asyncConnect`](https://github.com/mongodb/mongo/blob/master/src/mongo/transport/transport_layer.h#L85) as well.
[`TransportLayerManager`](../../transport/transport_layer_manager.h) provides these and
[`getReactor`](https://github.com/mongodb/mongo/blob/master/src/mongo/transport/transport_layer_manager.cpp#L78) as 
wrappers around synonymous functions in whichever [`SSLManager`](https://github.com/mongodb/mongo/blob/master/src/mongo/util/net/ssl_manager.h#L181)
the server is using.

### SNI

MongoDB uses a TLS feature called [**Server Name Indication (SNI)**](https://www.cloudflare.com/learning/ssl/what-is-sni/)
to transport server names over the network. The shell will use SNI to communicate what it believes the server's name is
when it initiates a connection to a server. SNI is a small, _optional_ parameter of a TLS handshake packet where a client
can advertize to a host what the client believes the host's name to be. SNI is normally used for hosts to know which 
certificate to use for an incoming connection when multiple domains are hosted at the same IP address. MongoDB also uses 
it often for communication between nodes in a replica set. TLS requires a client to initiate a handshake to a server, but 
_in replsets, two servers have to communicate with each other._ Because of this, _one server will take the role of a 
client_ and initiate the handshake to another server. In 
[**Split Horizon** routing](https://en.wikipedia.org/wiki/Split_horizon_route_advertisement), nodes will use SNI to 
figure out which horizon they are communicating on. Most cloud providers will allocate private, _internal network_ IP 
addresses that nodes can use to communicate with each other without reaching out to the internet, but each individual 
node will also have a public IP from which remote clients can access them. This is useful, for example, on deployments 
that have nodes across multiple cloud providers or across different regions that would not be able to exist on the same 
private network (horizon). When contacted by another node or client, a server will be able to tell the horizon on which 
it is communicating based on the name placed in the SNI field.

SNI also has the unfortunate property of being sent in the packet that _initiates_ a TLS connection. This means that 
_SNI cannot be encrypted_. This means that, if SNI is used, anyone who intercepts your packets can know with which host
specifically you are trying to interact.

## Protocol Theory

TLS works by having two peers with a _client/server relationship_ perform a handshake wherein they agree upon how 
messages will be encrypted and decrypted as they are sent between parties. Once the handshake completes
successfully, encrypted data will be sent across a regular transport protocol and then decrypted once it arrives on the
other side. For example, in the popular HTTPS protocol, a TLS handshake is negotiated, and then encrypted HTTP packets 
are formed into TLS records before being sent across the wire and decrypted on the other side.

### The TLS Handshake

As defined by TLS's specification, a TLS handshake will always consist of the following steps:

1. The client connects to a TLS-enabled server and presents a _list of supported **cipher suites**_ and supported 
_TLS versions_. This step is called the **Client Hello**.
2. The server will then _pick a cipher suite_ that it supports and send it to the client, along with its certificate.
This is called the **Server Hello**. The server's certificate will contain the server's _public key_, the server's _name_, 
and the _signature of a trusted Certificate Authority_.
3. The client will then **validate** the server certificate's signature against the CA's public key, which is stored
_on disk_ as a single certificate or as part of a certificate store. The client will also ensure that the server's 
certificate corresponds to the host to which it is trying to connect.
4. If the certificate is valid, then the client and server will perform a **key exchange** to generate a unique, random 
**session key**. This key is used for both encryption and decryption.
5. Using the session key, encrypted data is sent across the network.

### Ciphers

A **cipher** is an algorithm or function for turning cleartext data into an encrypted blob called a **ciphertext**.
A [**cipher suite**](https://en.wikipedia.org/wiki/Cipher_suite) is a named collection of algorithms to be used in a TLS
conversation. It contains, at the minimum, a _symmetric cipher_ for bulk encryption, and a _hash algorithm_. For TLS
versions older than 1.3, cipher suites also included a _key exchange algorithm_ for generating a session key, and an
_asymmetric signature algorithm_. A **forward-secret cipher suite** is a type of cipher suite that can be used to 
produce and exchange a forward-secret session key. **Forward-secrecy** means that even if a party's private key is 
compromised, previous sessions cannot be decrypted without knowing that session's key.
Any ciphers used by TLS 1.3 or newer have to be forward-secret ciphers. MongoDB supports two types of 
key-exchange algorithms that both provide forward-secrecy:

1. [Ephemeral Eliptic Curve Diffie-Hellman (ECDHE)](https://en.wikipedia.org/wiki/Elliptic-curve_Diffie–Hellman)
2. [Ephemeral Diffie-Hellman (DHE)](https://en.wikipedia.org/wiki/Diffie%E2%80%93Hellman_key_exchange)

If a client advertises both of these types of cipher suites, _the server will prefer ECDHE by default_, although this
can be re-configured. This is a property of the TLS libraries we use that we chose not to override.

One important requirement of a forward-secret system is that the forward-secret key is _never transmitted over the 
network,_ meaning that the client and server both have to derive the same forward-secret key independently. 

## X.509

[**X.509**](https://en.wikipedia.org/wiki/X.509) is a standard defining the format of a **public key certificate.**
X.509 certificates contain a _public key,_ an _identity,_ and a _signature,_ usually from a trusted Certificate Authority.

### Certificate Authorities

A **Certificate Authority (CA)** is a trusted third party used to definitevely bind names to public keys. When a service 
wants to be verified by a Certificate Authority, it will submit a **Certificate Signing Request (CSR)**, containing the 
service's identity and public key, to the Certificate Authority of its choosing. The CA will then cryptographically 
"sign" the certificate using its private key. When a MongoDB client receives a certificate, it will validate it using a CA 
certificate containing the CA's _public_ key. mongod, mongos, and mongo must be 
[configured](https://docs.mongodb.com/manual/tutorial/configure-ssl/#mongod-and-mongos-certificate-key-file) to use a CA
certificate **key file**, or the **system certificate store,** which is a keychain of trusted CA public keys which is 
common in various systems, services, browsers, etc. The keychain's set of public keys is typically refreshed via 
software updates. MongoDB clients can use the system keychain if a path to a certificate is not provided. This is used
most commonly by clients connecting to _Atlas clusters_. Certificate validation via a key file or the system keychain
both _do not require use of the network_.


### Certificate Metadata

Certificates are used to attach an identifier to a public key. There are two main types of identifiers that can be 
embedded in certificates: Common Name (CN) and Subject Alternative Name (SAN).

A **Subject Alternative Name (SAN)** allows DNS names or other identifying information to be embedded into a certificate. 
This information is useful for contacting adminstrators of the domain, or finding other information about the domain. A 
SAN can include any combination of the following:

* DNS names
* Email addresses
* IP addresses
* URIs
* Directory Names (DNs)
* General (arbitrary) names

A **Common Name (CN)** is allowed to _only_ contain hostnames.

Because of the richness of the information in a SAN, it is a much preferred identifier to CN. If the SAN is not 
provided in a certificate, the client will use the CN for validation; however, this is not ideal, as CN has been 
deprecated [since May 2000](https://tools.ietf.org/html/rfc2818#section-3.1).

### Certificate Expiration and Revocation

All certificates can be set to expire on an exact date. When the 
certificate expires, the certificate has to be renewed by the service. Ideally, the service will renew the certificate before it expires. 
Signatures can also be manually **revoked** by the CA before the expiration date if the certificate's private key 
is compromised. Traditionally, this is done through a **Certificate Revocation List (CRL)**. This is a list, stored
on disk, of certificates that have been revoked by a CA. These lists have to be re-downloaded periodically in order to 
know which certificates have recently been revoked. The alternative to this is the 
[**Online Certificate Status Protocol (OCSP)**](https://en.wikipedia.org/wiki/Online_Certificate_Status_Protocol) which 
allows certificate revocation to be checked online. OCSP is 
[supported by MongoDB](https://docs.mongodb.com/manual/core/security-transport-encryption/#ocsp-online-certificate-status-protocol).

### Member Certificates

MongoDB uses two types of certificates:

* **Client certificates**, which are used by clients to authenticate to a server.
* **Member certificates**, which are used by members of a _sharder cluster_ or _replset_ to authenticate to each other.

Member certificates are specified with 
[`net.tls.clusterFile`](https://docs.mongodb.com/manual/reference/configuration-options/#net.tls.clusterFile). This 
parameter points to a certificate key file (usually in .pem format) which contains both a certificate and a private key.
All member certificates in a cluster _must be issued by the same CA._ Members of a cluster will present these 
certificates on outbound connections to other nodes and authenticate to each other in the same way that a client would 
authenticate to them. If `net.tls.clusterFile` is not specified, then 
[`net.tls.certificateKeyFile`](https://docs.mongodb.com/manual/reference/configuration-options/#net.tls.certificateKeyFile) 
will be used.

By default, nodes will only consider a peer certificate to be a member certificate if the 
_Organization (O)_, _Organizational Unit (OU)_, and _Domain Component (DC)_ that might be contained 
in the certificate's _Subject Name_ match those contained in _its own_ subject name. This behavior 
can be customized to check for different attributes via `net.tls.clusterAuthX509.attributes` or 
`net.tls.clusterAuthX509.extensionValue`. See the [`auth`](../../db/auth/README.md) documentation 
for more information about X.509 intracluster auth.
