#!/bin/sh
# RNBO Runner exit cleanup — called by shim on overtake exit
kill $(pgrep rnbomovecontrol) 2>/dev/null
sleep 2
kill -9 $(pgrep -f "jackd.*shadow") 2>/dev/null
kill -9 $(pgrep rnbooscquery) 2>/dev/null
kill -9 $(pgrep jack_transport_link) 2>/dev/null
kill -9 $(pgrep rnbo-runner-pan) 2>/dev/null
