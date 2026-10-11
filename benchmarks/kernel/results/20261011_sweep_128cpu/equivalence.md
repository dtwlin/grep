# Benchmark Correctness and Equivalence Audit

## 1. Gate A: G1 vs G0 Byte-Exact Identity

| Scenario | Description | G0 Exit | G1 Exit | G0 Stdout SHA-256 | G1 Stdout SHA-256 | Gate A Verdict |
| :--- | :--- | :---: | :---: | :--- | :--- | :---: |

## 2. Gate B: Cross-Tool Normalized Match Set Equivalence

| Scenario | Description | R0 Match vs G0 | G2 Match vs G0 | X1 Match vs G0 | Notes |
| :--- | :--- | :---: | :---: | :---: | :--- |
| `B-01` |  | N/A | N/A | N/A | Missing run |
| `B-04` |  | N/A | N/A | N/A | Missing run |
| `M-02` |  | N/A | N/A | N/A | Missing run |
| `T-01` |  | N/A | N/A | N/A | Missing run |
| `T-04` |  | N/A | N/A | N/A | Missing run |
| `T-06` |  | N/A | N/A | N/A | Missing run |
| `T-10` |  | N/A | N/A | N/A | Missing run |
| `T-15` |  | N/A | N/A | N/A | Missing run |

### Diff Samples & Semantic Classifications

No diffs detected across normalized outputs.

## 3. Gate D: G1 Thread Invariance

G1 deterministic output verification across thread counts:

| Scenario | Description | Threads Checked | Identical Stdout SHA-256 | Gate D Verdict |
| :--- | :--- | :--- | :---: | :---: |
| `B-01` | rare literal ($BIG) | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `B-04` | DFA regex ($BIG) | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `M-02` | 1,000 literals, tree | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-01` | zero-match literal | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-04` | dense literal, line numbers | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-06` | files-with-matches | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-10` | word match | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |
| `T-15` | literal-free regex | 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128 | YES | **PASS** |

