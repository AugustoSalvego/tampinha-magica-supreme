# Tampinha Mágica Supreme

Professional MVP for an intelligent recycling credit system for schools.

The codebase is written in English, while the user interface is in Brazilian Portuguese because the system is designed for Brazilian teachers.

## Core product decision

The teacher is the main operator and administrator in the MVP. Students do not log into the system; they only use their RFID card. The machine stays with the teacher, and the teacher confirms each deposit.

## Main guarantees

- Online-first system.
- Offline-safe terminal behavior.
- No confirmed credit should be lost.
- Duplicate transaction protection using `transaction_uid`.
- Credits are never edited directly.
- Administrative corrections require a clear mandatory reason.
- Dangerous actions are protected by confirmation steps.
- Student registration remains simple: name, class, call number, and card.

## Project structure

```text
backend/       FastAPI, SQLite, dashboard, API, simulator
firmware/      ESP32 Arduino firmware draft
scripts/       helper scripts
docs/          product, UX and testing documentation
```

## Run locally

```bash
cd tampinha-magica-supreme/backend
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
uvicorn app.main:app --reload --host 0.0.0.0 --port 8000
```

Open:

```text
http://localhost:8000
```

Default login:

```text
Usuário: admin
Senha: admin123
```

## Seed demo data

```bash
cd backend
source .venv/bin/activate
python scripts/seed_demo.py
```

## Simulate the machine without ESP32

```bash
cd backend
source .venv/bin/activate
python scripts/simulate_machine.py
```

Demo RFID cards:

```text
A1B2C3D4 - João Silva
B1C2D3E4 - Maria Oliveira
C1D2E3F4 - Pedro Santos
```

## Important production note

This MVP is designed carefully, but the final system still needs real hardware validation, backup strategy, deployment hardening, HTTPS, and field tests with the school network before being used as the only production record.
