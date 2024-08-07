ro-ro-tcp
=========

`ro-ro-tcp` is a performance enhancing proxy for TCP. The main idea is
to split a TCP connection over several connections to achieve higher
bandwidth over links with high latency and moderate error rates.

The name comes from [Ro-Ro][] which stands for *Roll-On, Roll-Off*
which is a kind of railway station where trains are balanced accross
several tracks before entering the station to ensure a high
bandwidth. A [small video][] made with OpenTTD shows how this works
(except there is no packet reordering).

[Ro-Ro]: http://wiki.openttd.org/Railway_station#Ro-Ro
[small video]: http://www.dailymotion.com/video/x1dbe95_illustration-of-how-ro-ro-tcp-works_tech

A kernel 2.6.27.13 or later is required: `ro-ro-tcp` makes use of data
splicing and doesn't work around bugs in earlier kernel versions.

Warning
-------

This is work in progress and currently just doesn't work. Splicing is
overly complex to handle in a non-blocking way and I had a hard time
to grasp how it should have worked. The code is published just in case
but it really doesn't work.

[tcpsplicer][] is an implementation of a similar idea in Go. There is also
[wdt][] in C++.

[tcpsplicer]: https://github.com/pyke369/tcpsplice
[wdt]: https://github.com/facebook/wdt

Installation
------------

Execute the following commands:

    $ ./configure
    $ make
    $ sudo make install

Roles
-----

`ro-ro-tcp` handle two roles: proxy and relay. A _client_ will connect
to the _proxy_ instead of connecting directly to the server. The
_proxy_ will open multiple connections to the _relay_ which will in
turn open a connection to the _server_. Once the whole chain is
established, data can be transmitted both ways.

Protocol
--------

To split one TCP connection over several TCP connections between the
_proxy_ and the _relay_, we use two protocols:

 1. Establishment protocol
 2. Transmission protocol

The _establishment protocol_ happens when the proxy need to open
connections to the relay when a new client is connected. Let's say
that the proxy wants to open **N** connections to the relay. Here is
the protocol followed by the proxy:

 1. The proxy opens the first connection to the relay and waits for
    the connection to be established.
 2. The proxy sends 0 (encoded as a network-ordered unsigned 32-bit
    value).
 3. The proxy expects to receive its connection number, a
    network-ordered unsigned 32-bit value different of 0. This
    connection number is bound to the client the proxy accepted
    connection from.
 4. For each of the remaining connection with the relay to be opened:
     - The proxy establish a connection to the relay and waits for the
       connection to be established.
     - The proxy sends the connection number obtained from the first
       connection (encoded as a network-ordered unsigned 32-bit
       value).
     - The proxy expects to receive the same number.

In case of errors, all related connections are torn down. There is no
error signaling in this protocol and no way to recover. The relay
follows this protocol when it receives a connection from a proxy:

 1. It expects a connection number (a network-ordered unsigned 32-bit
    value).
 2. If the connection number is 0, it picks a new unused connection
    number and establish a connection to the server. It sends the
    connection number to the proxy.
 2. If the connection number is not 0, it searches for group of
    connection that already uses the same connection number and
    associates the new connection to this group. If found, it echoes
    back the connection number.

In case of errors, the connection with the proxy is terminated.

The _transmission protocol_ happens once the proxy and the relay
connections have been established for a given client. Each time the
proxy (resp. the relay) receives a new datagram from the client
(resp. the server), it will transmit it to the relay (resp. the proxy)
using this protocol over one of the connection opened (preferably with
some kind of load balancing).

The transmission protocol uses a fixed header of two unsigned 16-bit
value:
 - a serial number which is incremented for each datagram
 - the size of the datagram to be transmitted

The serial number ensures that the datagrams are delivered in the
appropriate order. The first serial number to be transmitted is 1.
