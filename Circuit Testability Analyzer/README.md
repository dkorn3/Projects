Circuit Testability Analyzer

A Python tool for analyzing digital circuits using logic simulation, SCOAP controllability, and Monte Carlo simulation.

Features:
Supports AND, OR, NAND, NOR, XOR, NOT, and BUF gates
Simulates .bench circuits
Calculates SCOAP c0 and c1 values
Estimates node probabilities using Monte Carlo simulation

Usage:

Place your .bench file at:

/content/c432.bench

Run:

python circuit_analyzer.py

The program outputs:

SCOAP Results — controllability of each node
Monte Carlo Results — probability of each node being 0 or 1
Input Format
INPUT(1)
INPUT(2)
OUTPUT(3)

3 = AND(1, 2)
Project Structure
circuit-testability-analyzer/
├── CircuitTestabilityAnalyzer.py
└── README.md
