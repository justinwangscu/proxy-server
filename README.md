# HTTP Forward Proxy Cache Server
An example HTTP Forward Proxy Server that intercepts, parses and forwards HTTP requests and responses between the browser and the web, caching responses and sending cached responses when applicable. Written in C.

## How to Compile the Project

Setup on Windows:
1. Install WSL (Windows Subsystem for Linux) and a distro - https://learn.microsoft.com/en-us/windows/wsl/install
2. Open the project directory in WSL. There are many ways to do this such as Shift + Right-clicking on an empty space with the folder open on File Explorer and selecting "Open Linux shell here". You could also navigate to the directory in Powershell or Command Prompt and run: `` wsl ``
3. Install gcc if not installed and optionally make.

Setup on Linux:
1. Install gcc if not installed and, optionally, install make.
2. ``cd`` to the project directory.

Compile automatically by running one of the following:
```bash
make
```
```bash
make all
```
```bash
./compile.sh
```

Compile Manually:
```bash
gcc -g main.c server.c -fsanitize=address -pthread -o server 
```

## How to Setup the Browser to Connect to the Proxy
Firefox is recommended since the proxy can be configured without changing system settings on the OS.
1. Open Firefox.
2. Click on the icon with three horizontal bars near the top right of the window. A dropdown menu should appear.
3. Click on "Settings" near the bottom of the menu.
4. You should be taken to the "General" tab as seen on the menu left of the screen. If not, click "General" on the menu on the left.
5. Scroll down to "Network Settings" and click the "Settings" underneath.
6. Select "Manual proxy configuration".
7. In the "HTTP Proxy" textbox, enter "127.0.0.1" if it is running locally. If the server is running on a different computer on the same network, enter the local IP of that machine instead. If the server is running on a computer on a different network enter the public IP instead (you may need to port forward).
8. Click "OK".

## Running the Server
``` 
./server
```

## Closing the Server
Ctrl + C -> Ctrl + C

## Changing the Port Number
Change the number in the following line in main.c:
```C
#define SERVER_PORT		3003		// Port number of server
```
Then compile again.

## Known Issues

"Bind failed!: Address already in use" errors due to improper closing.

Connecting to a different machine was not tested.

When the server responds with a cached file, the file the browser receives is sometimes corrupted or incomplete.

Closing the server takes two Ctrl + C's.

Inconsistent naming conventions.

No unit tests.
