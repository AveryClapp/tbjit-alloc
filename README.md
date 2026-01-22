# Trace-Based JIT Compiler

A speculative, adaptive compiler that watches bytecode execution, identifies hot paths, compiles them to x64 machine code, and falls back to interpretation when assumptions fail.

## What This Is

You're implementing the core technology behind PyPy and LuaJIT. The system observes program behavior at runtime, records linear execution traces of hot loops, compiles those traces to native code with guard conditions, and deoptimizes back to the interpreter when speculation fails.

This is low-level, research-grade systems work. You'll implement runtime code generation, handle speculation vs. correctness, and work directly at the code-to-machine boundary.

## Architecture

```
Run in interpreter → detect hot loop → record trace → compile to x64 with guards
                                                              |
                                                              v
                                        guard fails? → fallback to interpreter
```

**Core components:**
- **Scout/Interpreter:** Executes bytecode, observes runtime behavior, serves as fallback
- **Trace Recorder:** Captures linear instruction sequences for hot paths with runtime assumptions
- **Trace Compiler:** Converts recorded traces to x64 machine code with guard insertion
- **Guard & Deopt:** Falls back to interpreter when assumptions violated, optionally records new trace
- **Trace Chaining:** (Future) Direct linking of compiled traces for maximum performance

## 10-Day Roadmap

**Days 1-3: Tiny Bytecode VM**
- Stack-based VM with ~8 opcodes (LOAD_CONST, ADD, SUB, MUL, JUMP, JUMP_IF_ZERO, PRINT, HALT)
- Simple fetch-decode-execute loop
- Test: compute `sum = 0; for(i=0; i<100; i++) sum += i;`

**Days 4-6: Hot Path Detection**
- Backward jump counters at loop headers
- Trace recording infrastructure when threshold hit
- Milestone: print "HOT LOOP DETECTED" and log executed opcodes

**Days 7-10: Trace to x64**
- mmap executable memory buffer
- Generate machine code for ADD opcode
- Milestone: execute compiled trace, even if hardcoded

**After Day 10:**
- Guard insertion for type/value assumptions
- Deoptimization back to interpreter
- Full opcode coverage
- Register allocation (linear scan)
- Trace chaining

## Why This Matters

This isn't another compiler project. You're building adaptive optimization infrastructure that:
- Speculatively compiles based on observed behavior
- Maintains correctness through runtime guards
- Handles both fast path (compiled) and slow path (interpreted)
- Can extend into research territory (adaptive trace selection, novel guard optimization, trace merging)

The techniques here are used in production VMs handling billions of requests daily.

## Tech Stack

- C/C++ for VM and codegen
- x64 assembly for machine code emission
- mmap for executable memory management
- Zero dependencies for core implementation

## Getting Started

Write the interpreter first. Get something running. Everything else builds on that foundation.

The dopamine hit of watching your generated machine code execute will carry you through the rest.

---

**Status:** In development  
**Timeline:** Incremental progress, chip away approach  
**Goal:** Working trace compilation with guards and deopt
