# NetMessenger (IE3010 Assignment)

## Overview
NetMessenger is a client–server messaging application implemented in C.  
It supports multiple clients concurrently using `select()` I/O multiplexing.  
The system implements mandatory protocol commands including REGISTER, LIST, BCAST, PMSG, SENDFILE, and QUIT, with personalised logging and storage paths.

---

## Features
- **Concurrent server** using `select()` (no threads required).
- **Protocol commands**:
  - REGISTER: Register a new user.
  - LIST: List all connected users.
  - BCAST: Broadcast a message to all users.
  - PMSG: Send a private message to a specific user.
  - SENDFILE: Transfer files between clients.
  - QUIT: Disconnect gracefully.
- **Error handling** with codes:
  - ERR 001 USERNAME_TAKEN
  - ERR 002 USER_NOT_FOUND
  - ERR 003 ROOM_NOT_FOUND
- **Personalisation**:
  - Port: `7948`
  - Log file: `netmsg_IT19131948.log`
  - Storage path: `./storage/IT19131948/`

---

## Architecture
- **Server**: `server_1948.c`  
  Handles connections, concurrency, protocol parsing, logging, and file storage.
- **Client**: `client_1948.c`  
  Provides command‑line interface for sending protocol commands to the server.

---

## How to Run
1. Compile the server and client:
   ```bash
   gcc server_1948.c -o server_1948
   gcc client_1948.c -o client_1948
