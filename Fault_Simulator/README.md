Fault Simulator
Overview

A Python-based digital circuit fault simulator that reads .bench circuit files, simulates logic circuits, injects stuck-at faults, and measures fault coverage.

Features
.bench circuit parsing
Logic gate simulation
Stuck-at-0 and stuck-at-1 fault simulation
Fault detection
Fault coverage calculation
Single-fault and all-fault simulation
Usage

Update the circuit file in fault_simulator.py:

with open("/content/p2.bench", "r") as f:

Run:

python fault_simulator.py

Enter a test vector and choose single-fault or all-fault simulation.

Fault Coverage

Fault coverage is calculated as:

Detected Faults / Total Faults × 100
