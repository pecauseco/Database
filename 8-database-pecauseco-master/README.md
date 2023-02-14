# Database

How to use this code:
To begin, use "make clean all" in the directory of the folder for the project
Then run "./server <number>" with <number> being a number for a port between 3000 and 10000 such as 3333
Then in a separate terminal in the same directory for the project, type "./client localhost <number>" with <number> once again being the port number from before
You can create as many clients as necessary, and each can manipulate data in the database at the same time due to mutex locking
To disconnect a client, use CTRL C in the client's terminal
To suspend a client or database, use CTRL Z in the client or database's terminal respectively
To terminate all client's and exit the database use CTRL \ in the database's terminal
When accessing the database, the clients can perform four different types of actions:
a <key> <value>: Adds <key> into the database with value <value>.
q <key>: Retrieves/queries the value stored with key <key>.
d <key>: Deletes the given key and its associated value from the database.
f <file>: Executes the sequence of commands contained in the specified file, such as one of the provided scripts.
Additionally, the database can perform 2 commands on the clients:
s: stops all the clients
g: restarts all the clients after they've been stopped
The database is entirely thread safe due to various mutexes and writing/reading locks, allowing thousands of clients to edit the database simultaneously.


Overall structure of your code:
The structure of my code begins with the server.c file. This file runs a main method that creates a listener thread that then accepts new clients. When a new client is run, they run the run_client method which keeps running until a client disconnects, or are told to wait, which causes them to pause in the middle of the run client function. I also have a REPL for the server which will take in commands and perform functions based on these commands. I furthermore have a signal handler thread that will handle signals from SIGINT which cause it to terminate all threads currently running. After an EOF has been given and the server needs to exit, it will exit the REPL, make sure the server is no longer accepting commands and then commence its deletion protocol which calls signal handler destructors, destroys mutexes, cond variables, etc. and cleans up the database, all called under the while loop in the main method. The client runs different commands in db.c which are now thread safe that allow the client to print, Overall, this program can accept clients, allow them to perform functions on the database as well as allow the server to take in input and stop, go, or print leading to a complete database!
