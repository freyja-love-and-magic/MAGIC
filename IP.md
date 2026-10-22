As originally conceived, I called this MAGIC as a backronym for Multi-device Asynchronous Generic Input/output Consensus. The lawyers thought the patent office would balk at that description. Apparently I'm not the only inventor to believe I've created MAGIC. But I will say, it's a pretty good backronym.

**SYSTEMS AND METHODS FOR DYNAMICALLY CONFIGURING A WIRELESS
COMMUNICATIONS DEVICE TO TRANSMIT A PAYLOAD BASED ON A CONDITION AND
CONFIGURING ANOTHER DEVICE TO PERFORM AN OPERATION UPON RECEIVING THE
PAYLOAD**

## TECHNICAL FIELD

The present disclosure relates to systems and methods for facilitating electronic
communications between devices. In particular, the present disclosure relates to systems and
methods for dynamically configuring a device to transmit a payload based on a condition and
dynamically configuring another device to perform an operation upon receiving the payload.

## BACKGROUND

The next wave of connected devices will see a proliferation of small-scale computing
devices that are able to connect through short-lived, transient connections (e.g., handshakes).
Such a trend is already evident in “wearable” markets, most notably with smartwatches and
Bluetooth-enabled “smart” headphones. As these devices grow in popularity, there will likely be
a proliferation of two-way communication protocols that leverage various wireless mediums.

## SUMMARY

Electronic communications from one device to another are input to the second device. Input causes a change in state, and even if the input be ultimately rejected, this work is still work done by the receiving device. Insomuch that work done has a cost, systems will want a way to minimize unneeded or unwanted work. Since systems cannot control requests coming in from devices, a mechanism for establishing trust between devices is preferred. Traditionally this was done by an assumption of well-behaved humans, but the internet doesn't really have that assumption anymore.

The traditional Web topology was to present an offering, in this case as device two, to device one with the understanding that device one was controlled by a human. This just sort of worked because humans weren't proliferated automated device ones, but now that that's not just possible, but lucrative, the assumption of humanity has broken down and a new mechanism of trust is necessary. 

Trust is a sunk cost in disparate systems. A good actor on one website may be a bad actor on another, and since identities don't proliferate laterally between systems, a good actor everywhere becomes a non-actor everywhere else. This was a problem that cookies was trying to solve in the earliest days of the Web, but that quickly became usurped once everyone realized you could just track a user around the internet and advertise to them.

Instead it would be great to maintain some kind of network of trust without having to rebuild it everytime you want to use a new service as both devices one and two. As device one it would be great to understand the value of device two through this network of trust, and for device two it would be great to block requests from untrusted resources. 

In order to accomplish this, you need to throttle devices lest some gaming of the system compromises the trust in the network, and a way of forwarding requests with a known identifier to a trusted authority. In the README.md of this repo is how this is similar to the existing payment system, and indeed if everyone was willing to just pay for things online, a lot of this would be taken care of.

Fortunately for all of us, most of us don't have money to be spending, so we need to invent MAGIC in order to take care of it. So you want a throttle, money is a throttle since when it runs out you can't buy things anymore. Similarly magic points are a throttle because when you run out you can't cast things anymore. A simple single line check in a reverse proxy like NGINX can shut down a huge percentage of bad traffic from port scanning, and bots looking for old wordpress versions with a simple header check for the opting in to a made-up point system. 

The trust authority need not be a real-world entity beyond some guarantee of persistence, or perhaps even that's not needed if some sort hashed ordinal is kept, but some mechanism for checking trust. On the Web, trust goes from server to client, presumably via TLS, and there is no trust from client to server. 

This is an abstraction of the client to server relationship with arbitrary devices establishing trust between each other. Devices still represent users, but with a slightly more trustworthy assumption of reasonable rates of requesting. 
