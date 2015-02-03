# simple-web-server
A simple web server based on the thread pool model
This was a partner project

This project is a simple HTTP web server. The server can handle both
IPv4 and IPv6. The server is based on the thread pool model. There is 
a continual server loop that handles incoming connections. Every time it 
gets one it passes it off to the thread pool for client handling. This
way the server can handle multiple clients simultaneously. The sever 
also implements persistent connections following the HTTTP/1.1 protocol.
The server is robust and can handle errors. It should be able to run
for an indefinite amount of time. 
