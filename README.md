# Data Communication and Computer Networks

## mTCP Protocol

client folder

    -client.c       : the skeleton program provided
    
    -Makefile       : the makefile to compile the client program
    
    -mtcp_client.c  : the mtcp client library codes
    
    -mtcp_client.h  : header file for the mtcp client library codes
    
    -mtcp_common.h  : header file for definitions of some constants (we modified SERVER_PORT to 65534)

server folder 

    -server.c       : the skeleton program provided
    
    -Makefile       : the makefile to compile the server program
    
    -mtcp_server.c  : the mtcp server library codes
    
    -mtcp_server.h  : header file for the mtcp server library codes
    
    -mtcp_common.h  : header file for definitions of some constants (we modified SERVER_PORT to 65534)

Compilation:

    compile server program:
    
        cd csci4430/server
        
        make server

    compile client program:
    
        cd csci4430/client
        
        make client

Execution:

    1. Execute server program first
    
        cd csci4430/server
        
        ./server <server_ip_addr> <output_filename>

    2. Execute client program
    
        cd csci4430/client
        
        ./client <server_ip_addr> <file_to_send>

Clean:

    1. cd csci4430/server
    
    2. make clean
    
    3. cd csci4430/client
    
    4. make clean
