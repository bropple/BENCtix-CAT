#!/bin/sh
# What the demo console (tty7) shows: a message line, then a password line
# with echo off, so the indicator's '*' count is visible.
export TERM=linux
clear
printf '\n Linux on the Cat S22 Flip\n keypad text input\n\n'
printf ' message: '
read -r msg
printf ' password: '
stty -echo
read -r pw
stty echo
printf '\n\n you typed: %s\n password: %s characters, never shown\n' "$msg" "${#pw}"
sleep 600
