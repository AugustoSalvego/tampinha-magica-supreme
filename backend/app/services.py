"""Business rules for transactions, audit and device validation."""

from __future__ import annotations

from datetime import datetime
from sqlalchemy import or_, func
from sqlalchemy.orm import Session
from fastapi import HTTPException, status

from .models import AdminUser, AuditLog, Device, Student, Transaction, now_utc
from .security import verify_password


def audit(db: Session, actor: str, action: str, entity: str, entity_id: str | int | None = None, details: str | None = None) -> None:
    """Record an immutable audit log entry."""
    db.add(AuditLog(actor=actor, action=action, entity=entity, entity_id=str(entity_id) if entity_id is not None else None, details=details))


def validate_device(db: Session, device_id: str, device_key: str) -> Device:
    """Validate terminal credentials and return the active device."""
    device = db.query(Device).filter(Device.device_id == device_id).first()
    if not device or not device.active or not verify_password(device_key, device.device_key_hash):
        raise HTTPException(status_code=status.HTTP_401_UNAUTHORIZED, detail="Maquininha não autorizada")
    device.last_seen_at = now_utc()
    return device


def normalize_rfid(uid: str | None) -> str | None:
    """Normalize RFID input to avoid accidental mismatch caused by spaces/case."""
    if uid is None:
        return None
    cleaned = uid.strip().upper().replace(" ", "")
    return cleaned or None


def create_deposit_from_device(db: Session, payload) -> dict:
    """Create or acknowledge a device transaction.

    The function is idempotent: if the same transaction_uid is received twice,
    the already stored transaction is returned without adding credits again.
    """
    device = validate_device(db, payload.device_id, payload.device_key)

    existing = db.query(Transaction).filter(Transaction.transaction_uid == payload.transaction_uid).first()
    if existing:
        student = db.get(Student, existing.student_id)
        return {
            "ok": True,
            "duplicated": True,
            "message": "Transação já sincronizada. Nenhum crédito duplicado foi lançado.",
            "student_name": student.name if student else None,
            "student_id": student.id if student else None,
            "total_credits": student.total_credits if student else None,
        }

    if payload.weight_grams <= 0:
        raise HTTPException(status_code=400, detail="O peso precisa ser maior que zero")
    if payload.credits != payload.weight_grams:
        raise HTTPException(status_code=400, detail="A regra atual exige 1 grama = 1 crédito")

    rfid_uid = normalize_rfid(payload.rfid_uid)
    student = db.query(Student).filter(Student.rfid_uid == rfid_uid, Student.active.is_(True)).first()
    if not student:
        raise HTTPException(status_code=404, detail="Cartão não vinculado a nenhum aluno ativo")

    transaction = Transaction(
        transaction_uid=payload.transaction_uid,
        student_id=student.id,
        device_id=device.device_id,
        rfid_uid=rfid_uid,
        kind="DEPOSIT",
        source="DEVICE",
        weight_grams=payload.weight_grams,
        credits=payload.credits,
        status="SYNCED",
        created_at_device=payload.created_at_device,
        synced_at=now_utc(),
    )
    student.total_credits += payload.credits
    db.add(transaction)
    audit(db, "device:" + device.device_id, "deposit_synced", "transaction", payload.transaction_uid, f"Aluno={student.name}; créditos={payload.credits}")
    db.commit()
    db.refresh(transaction)
    return {
        "ok": True,
        "duplicated": False,
        "message": "Transação sincronizada com sucesso",
        "student_name": student.name,
        "student_id": student.id,
        "total_credits": student.total_credits,
        "transaction_id": transaction.id,
    }


def create_correction(db: Session, admin: AdminUser, student: Student, credits: int, reason: str) -> Transaction:
    """Create an administrative correction.

    Corrections can be positive or negative, but always require a clear reason.
    The student's total is adjusted by adding a new immutable transaction.
    """
    cleaned_reason = (reason or "").strip()
    if not cleaned_reason or len(cleaned_reason) < 10:
        raise ValueError("O motivo da correção precisa ser claro e ter pelo menos 10 caracteres.")
    if credits == 0:
        raise ValueError("A correção não pode ser zero.")
    if student.total_credits + credits < 0:
        raise ValueError("A correção deixaria o aluno com créditos negativos.")

    uid = f"admin-correction-{student.id}-{int(datetime.utcnow().timestamp() * 1000)}"
    transaction = Transaction(
        transaction_uid=uid,
        student_id=student.id,
        kind="CORRECTION",
        source="ADMIN",
        weight_grams=0,
        credits=credits,
        status="SYNCED",
        reason=cleaned_reason,
        admin_id=admin.id,
        synced_at=now_utc(),
    )
    student.total_credits += credits
    student.updated_at = now_utc()
    db.add(transaction)
    audit(db, admin.username, "administrative_correction", "student", student.id, f"Créditos={credits}; motivo={cleaned_reason}")
    db.commit()
    db.refresh(transaction)
    return transaction


def search_students(db: Session, query: str | None, active_filter: str = "active", classroom_id: int | None = None):
    """Return students filtered by search text, active status and classroom."""
    q = db.query(Student)
    if active_filter == "active":
        q = q.filter(Student.active.is_(True))
    elif active_filter == "inactive":
        q = q.filter(Student.active.is_(False))
    if classroom_id:
        q = q.filter(Student.classroom_id == classroom_id)
    if query:
        text = f"%{query.strip()}%"
        q = q.filter(or_(Student.name.ilike(text), Student.call_number.ilike(text), Student.rfid_uid.ilike(text), Student.notes.ilike(text)))
    return q.order_by(Student.name.asc()).all()


def find_possible_duplicates(db: Session, name: str, classroom_id: int | None, call_number: str | None):
    """Find likely duplicate students before creating a new record."""
    cleaned_name = name.strip()
    possible = []
    if cleaned_name:
        possible.extend(db.query(Student).filter(Student.name.ilike(f"%{cleaned_name}%"), Student.active.is_(True)).limit(5).all())
    if classroom_id and call_number:
        possible.extend(db.query(Student).filter(Student.classroom_id == classroom_id, Student.call_number == call_number, Student.active.is_(True)).limit(5).all())
    unique = {s.id: s for s in possible}
    return list(unique.values())
