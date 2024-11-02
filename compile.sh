#! /bin/bash

gcc -g main.c server.c -fsanitize=address -pthread -o server