# Database
Overall structure of your code:
The structure of my code is that I have a server.c file that runs a main method that creates a listener thread that then accepts new clients. When a new client is run, they run the run client method which they keep running until they disconnect, or are told to wait, which causes them to pause in the middle of the run client function. I also have a REPL for the server which will take in commands and perform functions based on these commands. I furthermore have a signal handler thread that will handle signals from SIGINT which cause it to terminate all threads currently running. After an EOF has been given and the server needs to exit, it will exit the REPL, make sure the server is no longer accepting commands and then commence its deletion protocol which calls signal handler destructors, destroys mutexes, cond variables, etc. and cleans up the database, all called under the while loop in the main method. The client runs different commands in db.c which are now thread safe that allow the client to print, Overall, this program can accept clients, allow them to perform functions on the database as well as allow the server to take in input and stop, go, or print leading to a complete database! PS I loved this class and you guys were awesome TAs, and helped make really hard concepts for me pretty easy!
Helper functions and what they do:
None
Changes to any function signatures:
None, except changed node constructor to contain a rwlock
Unresolved bugs:
NA