"""Seed demo data for local presentation."""

from __future__ import annotations

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.append(str(ROOT))

from app.database import Base, engine, SessionLocal
from app.models import Classroom, Student, Device, AdminUser, Transaction, now_utc
from app.security import hash_password

Base.metadata.create_all(bind=engine)

db = SessionLocal()
try:
    if not db.query(AdminUser).filter(AdminUser.username == "admin").first():
        db.add(AdminUser(username="admin", name="Professor administrador", password_hash=hash_password("admin123")))
    if not db.query(Device).filter(Device.device_id == "terminal-01").first():
        db.add(Device(device_id="terminal-01", device_key_hash=hash_password("change-this-device-key"), name="Maquininha principal", location="Com o professor"))

    classrooms = {}
    for name in ["5º Ano A", "5º Ano B", "6º Ano A"]:
        classroom = db.query(Classroom).filter(Classroom.name == name).first()
        if not classroom:
            classroom = Classroom(name=name, school_name="Escola Demonstração")
            db.add(classroom)
            db.flush()
        classrooms[name] = classroom

    students = [
        ("João Silva", classrooms["5º Ano A"].id, "12", "A1B2C3D4", 142),
        ("Maria Oliveira", classrooms["5º Ano A"].id, "08", "B1C2D3E4", 95),
        ("Pedro Santos", classrooms["5º Ano B"].id, "03", "C1D2E3F4", 80),
        ("João Silva", classrooms["5º Ano A"].id, "18", "D1E2F3A4", 0),
    ]

    for name, classroom_id, call_number, rfid, credits in students:
        student = db.query(Student).filter(Student.rfid_uid == rfid).first()
        if not student:
            student = Student(name=name, classroom_id=classroom_id, call_number=call_number, rfid_uid=rfid, total_credits=0)
            db.add(student)
            db.flush()
        if credits and not db.query(Transaction).filter(Transaction.transaction_uid == f"seed-{rfid}").first():
            tx = Transaction(
                transaction_uid=f"seed-{rfid}",
                student_id=student.id,
                device_id="terminal-01",
                rfid_uid=rfid,
                kind="DEPOSIT",
                source="DEVICE",
                weight_grams=credits,
                credits=credits,
                status="SYNCED",
                created_at_device="seed",
                synced_at=now_utc(),
            )
            student.total_credits += credits
            db.add(tx)

    db.commit()
    print("Demo data created successfully.")
finally:
    db.close()
