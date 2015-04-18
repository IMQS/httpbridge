This is a minimal embeddable HTTP/1.1 server that talks to an httpbridge backend.

It is intended to be used by unit tests. The server is unsuitable for
internet-facing deployment because it cannot accept more than 63 simultaneous connections.