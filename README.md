## About The Project

*con* is a program that makes acquiring serial console to a remote device across a network easier regardless of where the serial console is attached, the protocol to use, and the TCP port to get to it.

I have an RS232 console server (Perle IOLAN SCR1618) that I plug devices like switches into and it allows me to make them accessible using telnet and ssh via different TCP ports. I have some servers with IPMI that support serial over LAN. And I have other systems (mainly ARM SBCs) that expose console on TTL serial pins that I connect to with USB UARTs to other servers and then make those available over the network using [ser2net](https://github.com/cminyard/ser2net). Keeping track of how to get serial console when I need it was hard!

I discovered SVCB DNS records that allow encoding this sort of information into a service that I was already running, and all I needed was a tool that could query it and use it and I'd be able to effectively say "give me console on box foo" and just get it... and thus this program was born.

## Dependencies

This program depends upon SVCB records for console access being properly defined. I use bind 9 but any nameserver that supports these records should work.

For the three supported protocols you'll need packages installed that provide their client programs. For telnet you'll need a `telnet` client (on Debian this is inetutils-telnet). For `ssh` use openssh-client (again on Debian). And for ipmi-sol, install `ipmitool`.

The program links against the `resolv` library which is part of the GNU C library distributition. I have never tried to build this against musl or on bsd or macos. You might need to fiddle around some there!

## Installation

Get the `con` binary into your `$PATH` somewhere. `sudo make install` from the source distribution will do this. Make sure it's executable.

## Configuring DNS

Maybe I'm wrong but I think getting the DNS records correctly setup is going to be the hardest part for most users. Here's an example scenario:

* Your local domain name is foo.example.com. You have your DHCP server and IPv6 router advertisements configured to supply this as your suffix search list (instructions not provided here).
* You have a switch (switch.foo.example.com) connected to a console server (console.foo.example.com) and available via ssh on its TCP port 10007.
* You have a server (server.foo.example.com) with an IPMI implementation and serial over LAN (server-ipmi.foo.example.com).
* You have a single board computer (sbc.foo.example.com) with its console pins connected to the server listed above (server.foo.example.com) via a USB UART and exposed via `ser2net` as TCP port 10010 over telnet.

Within your zone file for foo.example.com you would need records like the following:

```
_console.switch     IN      SVCB    1       console.foo.example.com.        port=10001  alpn="ssh"
_console.server     IN      SVCB    1       server-ipmi.foo.example.com.    port=623    alpn="ipmi-sol"
_console.sbc        IN      SVCB    1       server.foo.example.com.         port=10010  alpn="telnet"
```

`_console` as a prefix is what this program uses to identify the resource. It's not special within SVCB but is what this app chose. After that comes the name of the device that you want console on. Then the class - in this case `IN`. The type of the records are `SVCB` which refers to service binding. Then comes the fully qualified reference to the host that exposes the console - be mindful of the trailing dot! Lastly you encode the port and the application layer protocol negotiation (`alpn`) .

Three values for `alpn` are supported right now: `telnet`, `ssh`, and `ipmi-sol`. Any other values will cause errors when consumed by this program (but bind won't care when it loads the zone).

Once you have DNS records created I recommend testing it with `dig`. An example command for the switch example above is: `dig SVCB @yournameserver _console.switch.foo.example.com`. If that succeeds in the `ANSWER` section you should see something that looks close to what you put in the zone file.

## Usage

If that works, you're ready to try using this program! Invoke it like:

```con switch```

This assumes that your local resolver is configured with "foo.example.com" as a suffix to search for short names. `con` will prepend "_console." and then the local resolver will append ".foo.example.com." before resolving the name. This should then initiate an ssh connection via the hostname and port found in DNS and prompt you for a password (or not if you use a key). When you want to disconnect, if you type "exit" it'll log you out of the console connection via `getty` but it'll just ask for you to login again (because this is a serial console). Instead, you'll need to signal to ssh that you're done and you do that by pressing ENTER followed by a tilde and dot (~.). Note that if you have multiple ssh connections chained together (ssh to one host to the ssh to the serial port) you'll need to supply an extra tilde (~) for every host in the chain to ignore it so that the final one actually acts - otherwise the "outer" ssh connection will disconnect which may not be what you want.

For telnet connection exit with ctrl+] (control and right bracket). This will drop you to a "telnet>" prompt and you an then type "quit" or ctrl+d to get out.

`ipmitool` handles exiting similar to ssh: ~.

In any case, the `con` program replaces itself in memory with the appropriate network client and when that exits, you're back to your shell.

## To Do

Right now, nothing really. This does what I want.

If anyone reading this has other protocols that they want to access, send me a pull request (or ask me nicely).

I have considered how I could make disconnecting easier and I can't think of a way that wouldn't make the code really messy. But perhaps there is.

The only part of the code that I don't really like is where the array for the commands is built. Maybe I could clean that up.

## License

Public domain. See LICENSE file.

Any contributions made should be understood to follow this as well. If you don't like that, fork it and do whatever.

## Acknowledgements

AI was used minimally in the creation of this program. I asked Google for an example of how to query SVCB records which it did and then asked me a few more questions - and before I knew it I had a fully functional program (being honest this blew me away). Then I looked at the code and saw that it was (functional) garbage and went about rewriting the entire thing. I fixed a few bugs, removed an absurd amount of unnecessary code, addressed a security vulnerability, and otherwise cleaned it up to be at least minimally acceptable. I am a human and while I do not accept any responsibility for this code from a legal standpoint (see LICENSE) I will vouch that I wrote it myself when it's all said and done. 
