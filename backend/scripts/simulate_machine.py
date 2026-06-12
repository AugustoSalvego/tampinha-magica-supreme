"""Interactive terminal simulator for the ESP32 machine.

This script is useful while the physical modules are not available. It simulates
RFID reading, weighing and transaction synchronization.
"""

from __future__ import annotations

import time
import uuid
import requests

API_BASE = "http://localhost:8000"
DEVICE_ID = "terminal-01"
DEVICE_KEY = "change-this-device-key"


def get_student(rfid_uid: str) -> dict:
    return requests.get(f"{API_BASE}/api/students/by-rfid/{rfid_uid}", timeout=10).json()


def sync_transaction(rfid_uid: str, grams: int) -> dict:
    payload = {
        "transaction_uid": f"sim-{DEVICE_ID}-{int(time.time())}-{uuid.uuid4().hex[:8]}",
        "device_id": DEVICE_ID,
        "device_key": DEVICE_KEY,
        "rfid_uid": rfid_uid,
        "weight_grams": grams,
        "credits": grams,
        "created_at_device": "simulator",
    }
    response = requests.post(f"{API_BASE}/api/transactions/sync", json=payload, timeout=10)
    if not response.ok:
        return {"ok": False, "status": response.status_code, "detail": response.text}
    return response.json()


def main() -> None:
    print("Tampinha Mágica - Machine Simulator")
    print("Press Ctrl+C to exit. Demo RFID: A1B2C3D4, B1C2D3E4, C1D2E3F4")
    while True:
        rfid = input("\nRFID UID: ").strip().upper()
        if not rfid:
            continue
        student = get_student(rfid)
        if not student.get("found"):
            print("Cartão não vinculado a nenhum aluno ativo.")
            continue
        print(f"Aluno: {student['name']} | Turma: {student.get('classroom')} | Nº {student.get('call_number')} | Total: {student.get('total_credits')}")
        grams_text = input("Peso em gramas: ").strip()
        try:
            grams = int(grams_text)
        except ValueError:
            print("Peso inválido.")
            continue
        if grams <= 0:
            print("Peso precisa ser maior que zero.")
            continue
        confirm = input(f"Confirmar {grams} créditos para {student['name']}? [s/N] ").strip().lower()
        if confirm != "s":
            print("Cancelado.")
            continue
        result = sync_transaction(rfid, grams)
        print(result)


if __name__ == "__main__":
    main()
