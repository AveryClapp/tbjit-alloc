# Trace-Based JIT Compiler

A speculative, adaptive compiler that watches bytecode execution, identifies hot paths, compiles them to x64 machine code, and falls back to interpretation when assumptions fail.

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

## Tech Stack

- C/C++ for VM and codegen
- x64 assembly for machine code emission
- mmap for executable memory management
- Zero dependencies for core implementation

---
