"""Tampinha Mágica Supreme backend.

The web interface is intentionally in Brazilian Portuguese, while code and
comments remain in English.
"""

from __future__ import annotations

import csv
import shutil
from datetime import datetime
from pathlib import Path
from typing import Annotated

from fastapi import Depends, FastAPI, Form, HTTPException, Request, Response, status
from fastapi.responses import HTMLResponse, RedirectResponse, StreamingResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from sqlalchemy import func, or_
from sqlalchemy.orm import Session
from starlette.middleware.sessions import SessionMiddleware

from .database import Base, SessionLocal, engine, get_db, DATA_DIR
from .models import AdminUser, AuditLog, Campaign, Classroom, Device, Student, Transaction, now_utc
from .schemas import DevicePing, LastRfidPayload, SyncTransactionPayload
from .security import hash_password, verify_password
from .services import audit, create_correction, create_deposit_from_device, find_possible_duplicates, normalize_rfid, search_students, validate_device

APP_DIR = Path(__file__).resolve().parent
BASE_DIR = APP_DIR.parents[0]
BACKUP_DIR = DATA_DIR / "backups"
BACKUP_DIR.mkdir(parents=True, exist_ok=True)

app = FastAPI(title="Tampinha Mágica", version="2.0.0-supreme")
app.add_middleware(SessionMiddleware, secret_key="change-this-session-secret-before-production")
app.mount("/static", StaticFiles(directory=APP_DIR / "static"), name="static")
templates = Jinja2Templates(directory=APP_DIR / "templates")


def create_default_records() -> None:
    """Create tables and default admin/device for local MVP usage."""
    Base.metadata.create_all(bind=engine)
    db = SessionLocal()
    try:
        if not db.query(AdminUser).filter(AdminUser.username == "admin").first():
            db.add(AdminUser(username="admin", name="Professor administrador", password_hash=hash_password("admin123")))
        if not db.query(Device).filter(Device.device_id == "terminal-01").first():
            db.add(Device(
                device_id="terminal-01",
                device_key_hash=hash_password("change-this-device-key"),
                name="Maquininha principal",
                location="Com o professor",
                firmware_version="simulator",
            ))
        db.commit()
    finally:
        db.close()


@app.on_event("startup")
def startup() -> None:
    create_default_records()


def current_admin(request: Request, db: Session = Depends(get_db)) -> AdminUser:
    """Return the logged-in admin or redirect to login."""
    admin_id = request.session.get("admin_id")
    if not admin_id:
        raise HTTPException(status_code=status.HTTP_307_TEMPORARY_REDIRECT, headers={"Location": "/login"})
    admin = db.get(AdminUser, admin_id)
    if not admin or not admin.active:
        request.session.clear()
        raise HTTPException(status_code=status.HTTP_307_TEMPORARY_REDIRECT, headers={"Location": "/login"})
    return admin


def render(request: Request, name: str, context: dict | None = None, status_code: int = 200) -> HTMLResponse:
    ctx = {"request": request, "flash": request.session.pop("flash", None), "error": request.session.pop("error", None)}
    if context:
        ctx.update(context)
    return templates.TemplateResponse(name, ctx, status_code=status_code)


def flash_redirect(request: Request, url: str, message: str | None = None, error: str | None = None) -> RedirectResponse:
    if message:
        request.session["flash"] = message
    if error:
        request.session["error"] = error
    return RedirectResponse(url=url, status_code=303)


@app.get("/health")
def health():
    return {"ok": True, "service": "tampinha-magica", "version": "2.0.0-supreme"}


@app.get("/", response_class=HTMLResponse)
def home(request: Request):
    if request.session.get("admin_id"):
        return RedirectResponse("/dashboard", status_code=303)
    return RedirectResponse("/login", status_code=303)


@app.get("/login", response_class=HTMLResponse)
def login_page(request: Request):
    return render(request, "login.html")


@app.post("/login")
def login(request: Request, db: Annotated[Session, Depends(get_db)], username: str = Form(...), password: str = Form(...)):
    admin = db.query(AdminUser).filter(AdminUser.username == username.strip(), AdminUser.active.is_(True)).first()
    if not admin or not verify_password(password, admin.password_hash):
        return flash_redirect(request, "/login", error="Usuário ou senha inválidos.")
    request.session["admin_id"] = admin.id
    audit(db, admin.username, "login", "admin_user", admin.id, "Login realizado")
    db.commit()
    return flash_redirect(request, "/dashboard", message="Bem-vindo ao Tampinha Mágica.")


@app.get("/logout")
def logout(request: Request):
    request.session.clear()
    return RedirectResponse("/login", status_code=303)


@app.get("/dashboard", response_class=HTMLResponse)
def dashboard(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)]):
    total_students = db.query(func.count(Student.id)).filter(Student.active.is_(True)).scalar() or 0
    total_classrooms = db.query(func.count(Classroom.id)).filter(Classroom.active.is_(True)).scalar() or 0
    total_credits = db.query(func.coalesce(func.sum(Student.total_credits), 0)).filter(Student.active.is_(True)).scalar() or 0
    deposits_count = db.query(func.count(Transaction.id)).filter(Transaction.kind == "DEPOSIT").scalar() or 0
    corrections_count = db.query(func.count(Transaction.id)).filter(Transaction.kind == "CORRECTION").scalar() or 0
    online_devices = db.query(func.count(Device.id)).filter(Device.active.is_(True), Device.last_seen_at.isnot(None)).scalar() or 0
    recent = db.query(Transaction).order_by(Transaction.created_at.desc()).limit(8).all()
    top_students = db.query(Student).filter(Student.active.is_(True)).order_by(Student.total_credits.desc()).limit(5).all()
    classrooms = db.query(Classroom).filter(Classroom.active.is_(True)).order_by(Classroom.name.asc()).all()
    return render(request, "dashboard.html", {
        "admin": admin,
        "metrics": {
            "students": total_students,
            "classrooms": total_classrooms,
            "credits": total_credits,
            "kg": round(total_credits / 1000, 2),
            "deposits": deposits_count,
            "corrections": corrections_count,
            "devices": online_devices,
        },
        "recent": recent,
        "top_students": top_students,
        "classrooms": classrooms,
    })


@app.get("/students", response_class=HTMLResponse)
def students_page(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], q: str | None = None, status_filter: str = "active", classroom_id: int | None = None):
    classrooms = db.query(Classroom).filter(Classroom.active.is_(True)).order_by(Classroom.name.asc()).all()
    students = search_students(db, q, status_filter, classroom_id)
    return render(request, "students.html", {"students": students, "classrooms": classrooms, "q": q or "", "status_filter": status_filter, "classroom_id": classroom_id})


@app.get("/students/new", response_class=HTMLResponse)
def new_student_page(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)]):
    classrooms = db.query(Classroom).filter(Classroom.active.is_(True)).order_by(Classroom.name.asc()).all()
    return render(request, "student_form.html", {"classrooms": classrooms, "student": None, "duplicates": []})


@app.post("/students/preview")
def preview_student(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], name: str = Form(...), classroom_id: int = Form(...), call_number: str = Form(""), rfid_uid: str = Form(""), notes: str = Form("")):
    classrooms = db.query(Classroom).filter(Classroom.active.is_(True)).order_by(Classroom.name.asc()).all()
    duplicates = find_possible_duplicates(db, name, classroom_id, call_number.strip() or None)
    temp = {"name": name, "classroom_id": classroom_id, "call_number": call_number, "rfid_uid": rfid_uid, "notes": notes}
    return render(request, "student_form.html", {"classrooms": classrooms, "student": temp, "duplicates": duplicates, "preview": True})


@app.post("/students/create")
def create_student(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], name: str = Form(...), classroom_id: int = Form(...), call_number: str = Form(""), rfid_uid: str = Form(""), notes: str = Form("")):
    cleaned_rfid = normalize_rfid(rfid_uid)
    if cleaned_rfid and db.query(Student).filter(Student.rfid_uid == cleaned_rfid).first():
        return flash_redirect(request, "/students/new", error="Este cartão já está vinculado a outro aluno.")
    student = Student(name=name.strip(), classroom_id=classroom_id, call_number=call_number.strip() or None, rfid_uid=cleaned_rfid, notes=notes.strip() or None)
    db.add(student)
    db.flush()
    audit(db, admin.username, "student_created", "student", student.id, f"Aluno criado: {student.name}")
    db.commit()
    return flash_redirect(request, f"/students/{student.id}", message="Aluno cadastrado com sucesso.")


@app.get("/students/{student_id}", response_class=HTMLResponse)
def student_detail(request: Request, student_id: int, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)]):
    student = db.get(Student, student_id)
    if not student:
        raise HTTPException(404)
    classrooms = db.query(Classroom).filter(Classroom.active.is_(True)).order_by(Classroom.name.asc()).all()
    transactions = db.query(Transaction).filter(Transaction.student_id == student.id).order_by(Transaction.created_at.desc()).limit(20).all()
    audits = db.query(AuditLog).filter(AuditLog.entity == "student", AuditLog.entity_id == str(student.id)).order_by(AuditLog.created_at.desc()).limit(10).all()
    return render(request, "student_detail.html", {"student": student, "classrooms": classrooms, "transactions": transactions, "audits": audits})


@app.post("/students/{student_id}/edit")
def edit_student(request: Request, student_id: int, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], name: str = Form(...), classroom_id: int = Form(...), call_number: str = Form(""), notes: str = Form("")):
    student = db.get(Student, student_id)
    if not student:
        raise HTTPException(404)
    old = f"{student.name} / turma={student.classroom_id} / chamada={student.call_number}"
    student.name = name.strip()
    student.classroom_id = classroom_id
    student.call_number = call_number.strip() or None
    student.notes = notes.strip() or None
    student.updated_at = now_utc()
    audit(db, admin.username, "student_edited", "student", student.id, f"Antes: {old}; Depois: {student.name} / turma={classroom_id} / chamada={student.call_number}")
    db.commit()
    return flash_redirect(request, f"/students/{student.id}", message="Dados do aluno atualizados.")


@app.post("/students/{student_id}/link-card")
def link_card(request: Request, student_id: int, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], rfid_uid: str = Form(...), reason: str = Form(...)):
    student = db.get(Student, student_id)
    if not student:
        raise HTTPException(404)
    cleaned = normalize_rfid(rfid_uid)
    if not cleaned:
        return flash_redirect(request, f"/students/{student.id}", error="Informe ou leia um cartão válido.")
    if len(reason.strip()) < 8:
        return flash_redirect(request, f"/students/{student.id}", error="Informe um motivo claro para trocar/vincular o cartão.")
    other = db.query(Student).filter(Student.rfid_uid == cleaned, Student.id != student.id).first()
    if other:
        return flash_redirect(request, f"/students/{student.id}", error=f"Este cartão já pertence ao aluno {other.name}.")
    old = student.rfid_uid or "sem cartão"
    student.rfid_uid = cleaned
    student.updated_at = now_utc()
    audit(db, admin.username, "rfid_changed", "student", student.id, f"Cartão antigo={old}; novo={cleaned}; motivo={reason.strip()}")
    db.commit()
    return flash_redirect(request, f"/students/{student.id}", message="Cartão vinculado/alterado com segurança.")


@app.post("/students/{student_id}/deactivate")
def deactivate_student(request: Request, student_id: int, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], confirmation: str = Form(...), reason: str = Form(...)):
    student = db.get(Student, student_id)
    if not student:
        raise HTTPException(404)
    if confirmation.strip().upper() != "DESATIVAR":
        return flash_redirect(request, f"/students/{student.id}", error="Para desativar, digite DESATIVAR exatamente como solicitado.")
    if len(reason.strip()) < 10:
        return flash_redirect(request, f"/students/{student.id}", error="Informe um motivo claro para desativar o aluno.")
    student.active = False
    student.updated_at = now_utc()
    audit(db, admin.username, "student_deactivated", "student", student.id, reason.strip())
    db.commit()
    return flash_redirect(request, "/students?status_filter=inactive", message="Aluno desativado. O histórico foi preservado.")


@app.post("/students/{student_id}/reactivate")
def reactivate_student(request: Request, student_id: int, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], reason: str = Form(...)):
    student = db.get(Student, student_id)
    if not student:
        raise HTTPException(404)
    if len(reason.strip()) < 10:
        return flash_redirect(request, f"/students/{student.id}", error="Informe um motivo claro para reativar o aluno.")
    student.active = True
    student.updated_at = now_utc()
    audit(db, admin.username, "student_reactivated", "student", student.id, reason.strip())
    db.commit()
    return flash_redirect(request, f"/students/{student.id}", message="Aluno reativado.")


@app.get("/classrooms", response_class=HTMLResponse)
def classrooms_page(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], q: str | None = None):
    query = db.query(Classroom).filter(Classroom.active.is_(True))
    if q:
        query = query.filter(or_(Classroom.name.ilike(f"%{q}%"), Classroom.school_name.ilike(f"%{q}%")))
    classrooms = query.order_by(Classroom.name.asc()).all()
    return render(request, "classrooms.html", {"classrooms": classrooms, "q": q or ""})


@app.post("/classrooms/create")
def create_classroom(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], name: str = Form(...), school_name: str = Form("")):
    classroom = Classroom(name=name.strip(), school_name=school_name.strip() or None)
    db.add(classroom)
    db.flush()
    audit(db, admin.username, "classroom_created", "classroom", classroom.id, classroom.name)
    db.commit()
    return flash_redirect(request, "/classrooms", message="Turma cadastrada.")


@app.get("/classrooms/{classroom_id}", response_class=HTMLResponse)
def classroom_detail(request: Request, classroom_id: int, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)]):
    classroom = db.get(Classroom, classroom_id)
    if not classroom:
        raise HTTPException(404)
    students = db.query(Student).filter(Student.classroom_id == classroom.id, Student.active.is_(True)).order_by(Student.call_number.asc(), Student.name.asc()).all()
    total_credits = sum(s.total_credits for s in students)
    recent = db.query(Transaction).join(Student).filter(Student.classroom_id == classroom.id).order_by(Transaction.created_at.desc()).limit(10).all()
    return render(request, "classroom_detail.html", {"classroom": classroom, "students": students, "total_credits": total_credits, "recent": recent})


@app.post("/classrooms/{classroom_id}/edit")
def edit_classroom(request: Request, classroom_id: int, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], name: str = Form(...), school_name: str = Form("")):
    classroom = db.get(Classroom, classroom_id)
    if not classroom:
        raise HTTPException(404)
    old = classroom.name
    classroom.name = name.strip()
    classroom.school_name = school_name.strip() or None
    classroom.updated_at = now_utc()
    audit(db, admin.username, "classroom_edited", "classroom", classroom.id, f"{old} -> {classroom.name}")
    db.commit()
    return flash_redirect(request, f"/classrooms/{classroom.id}", message="Turma atualizada.")


@app.get("/transactions", response_class=HTMLResponse)
def transactions_page(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], q: str | None = None, kind: str = "all"):
    query = db.query(Transaction).join(Student)
    if kind != "all":
        query = query.filter(Transaction.kind == kind)
    if q:
        text = f"%{q}%"
        query = query.filter(or_(Student.name.ilike(text), Student.call_number.ilike(text), Student.rfid_uid.ilike(text), Transaction.transaction_uid.ilike(text), Transaction.reason.ilike(text)))
    transactions = query.order_by(Transaction.created_at.desc()).limit(250).all()
    students = db.query(Student).filter(Student.active.is_(True)).order_by(Student.name.asc()).all()
    return render(request, "transactions.html", {"transactions": transactions, "students": students, "q": q or "", "kind": kind})


@app.post("/corrections/create")
def create_correction_route(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], student_id: int = Form(...), credits: int = Form(...), reason: str = Form(...), confirm_understanding: str | None = Form(None)):
    student = db.get(Student, student_id)
    if not student:
        return flash_redirect(request, "/transactions", error="Aluno não encontrado.")
    if confirm_understanding != "yes":
        return flash_redirect(request, "/transactions", error="Confirme que você entende que a correção ficará registrada no histórico.")
    try:
        create_correction(db, admin, student, credits, reason)
    except ValueError as exc:
        return flash_redirect(request, "/transactions", error=str(exc))
    return flash_redirect(request, f"/students/{student.id}", message="Correção administrativa registrada com segurança.")


@app.get("/transactions/export.csv")
def export_transactions(db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)]):
    rows = db.query(Transaction).order_by(Transaction.created_at.desc()).all()
    def generate():
        yield "id,transaction_uid,student,kind,source,weight_grams,credits,status,reason,created_at\n"
        for tx in rows:
            student_name = tx.student.name if tx.student else ""
            yield f'{tx.id},{tx.transaction_uid},"{student_name}",{tx.kind},{tx.source},{tx.weight_grams},{tx.credits},{tx.status},"{(tx.reason or "").replace(chr(34), chr(34)*2)}",{tx.created_at.isoformat()}\n'
    return StreamingResponse(generate(), media_type="text/csv", headers={"Content-Disposition": "attachment; filename=tampinha-magica-transacoes.csv"})


@app.get("/ranking", response_class=HTMLResponse)
def ranking_page(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)]):
    students = db.query(Student).filter(Student.active.is_(True)).order_by(Student.total_credits.desc()).all()
    classrooms = db.query(Classroom).filter(Classroom.active.is_(True)).all()
    classroom_ranking = []
    for c in classrooms:
        total = sum(s.total_credits for s in c.students if s.active)
        classroom_ranking.append({"classroom": c, "total": total, "kg": round(total / 1000, 2)})
    classroom_ranking.sort(key=lambda x: x["total"], reverse=True)
    return render(request, "ranking.html", {"students": students, "classroom_ranking": classroom_ranking})


@app.get("/devices", response_class=HTMLResponse)
def devices_page(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)]):
    devices = db.query(Device).order_by(Device.name.asc()).all()
    return render(request, "devices.html", {"devices": devices})


@app.post("/devices/create")
def create_device(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], device_id: str = Form(...), device_key: str = Form(...), name: str = Form(...), location: str = Form("")):
    if db.query(Device).filter(Device.device_id == device_id.strip()).first():
        return flash_redirect(request, "/devices", error="Já existe uma maquininha com este código.")
    device = Device(device_id=device_id.strip(), device_key_hash=hash_password(device_key), name=name.strip(), location=location.strip() or None)
    db.add(device)
    db.flush()
    audit(db, admin.username, "device_created", "device", device.id, device.device_id)
    db.commit()
    return flash_redirect(request, "/devices", message="Maquininha cadastrada.")


@app.get("/campaigns", response_class=HTMLResponse)
def campaigns_page(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)]):
    campaigns = db.query(Campaign).order_by(Campaign.created_at.desc()).all()
    total_credits = db.query(func.coalesce(func.sum(Student.total_credits), 0)).filter(Student.active.is_(True)).scalar() or 0
    return render(request, "campaigns.html", {"campaigns": campaigns, "total_credits": total_credits})


@app.post("/campaigns/create")
def create_campaign(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)], name: str = Form(...), goal_grams: int = Form(...), description: str = Form("")):
    campaign = Campaign(name=name.strip(), goal_grams=max(goal_grams, 0), description=description.strip() or None)
    db.add(campaign)
    db.flush()
    audit(db, admin.username, "campaign_created", "campaign", campaign.id, campaign.name)
    db.commit()
    return flash_redirect(request, "/campaigns", message="Campanha cadastrada.")


@app.get("/audit", response_class=HTMLResponse)
def audit_page(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)]):
    logs = db.query(AuditLog).order_by(AuditLog.created_at.desc()).limit(300).all()
    return render(request, "audit.html", {"logs": logs})


@app.get("/backups", response_class=HTMLResponse)
def backups_page(request: Request, admin: Annotated[AdminUser, Depends(current_admin)]):
    backups = sorted(BACKUP_DIR.glob("*.db"), reverse=True)
    return render(request, "backups.html", {"backups": backups})


@app.post("/backups/create")
def create_backup(request: Request, db: Annotated[Session, Depends(get_db)], admin: Annotated[AdminUser, Depends(current_admin)]):
    source = DATA_DIR / "tampinha_magica.db"
    if not source.exists():
        return flash_redirect(request, "/backups", error="Banco de dados ainda não existe.")
    target = BACKUP_DIR / f"backup-{datetime.utcnow().strftime('%Y%m%d-%H%M%S')}.db"
    shutil.copy2(source, target)
    audit(db, admin.username, "backup_created", "database", target.name, "Backup manual gerado")
    db.commit()
    return flash_redirect(request, "/backups", message="Backup gerado com sucesso.")


@app.get("/help", response_class=HTMLResponse)
def help_page(request: Request, admin: Annotated[AdminUser, Depends(current_admin)]):
    return render(request, "help.html")


# ---------------- API used by the ESP32 terminal ----------------

@app.get("/api/students/by-rfid/{rfid_uid}")
def api_student_by_rfid(rfid_uid: str, db: Annotated[Session, Depends(get_db)]):
    uid = normalize_rfid(rfid_uid)
    student = db.query(Student).filter(Student.rfid_uid == uid, Student.active.is_(True)).first()
    if not student:
        return {"found": False, "message": "Cartão não vinculado a nenhum aluno ativo"}
    return {
        "found": True,
        "student_id": student.id,
        "name": student.name,
        "display_code": student.display_code,
        "classroom": student.classroom.name if student.classroom else None,
        "call_number": student.call_number,
        "total_credits": student.total_credits,
    }


@app.post("/api/devices/ping")
def api_device_ping(payload: DevicePing, db: Annotated[Session, Depends(get_db)]):
    device = validate_device(db, payload.device_id, payload.device_key)
    device.firmware_version = payload.firmware_version or device.firmware_version
    device.pending_count = payload.pending_count
    db.commit()
    return {"ok": True, "message": "Maquininha conectada", "server_time": now_utc().isoformat()}


@app.post("/api/devices/last-rfid")
def api_last_rfid(payload: LastRfidPayload, db: Annotated[Session, Depends(get_db)]):
    device = validate_device(db, payload.device_id, payload.device_key)
    device.last_rfid_uid = normalize_rfid(payload.rfid_uid)
    device.last_rfid_at = now_utc()
    audit(db, "device:" + device.device_id, "rfid_read", "device", device.device_id, device.last_rfid_uid)
    db.commit()
    return {"ok": True, "message": "Cartão lido"}


@app.get("/api/devices/last-rfid")
def api_get_last_rfid(db: Annotated[Session, Depends(get_db)]):
    device = db.query(Device).order_by(Device.last_rfid_at.desc().nullslast()).first()
    if not device or not device.last_rfid_uid:
        return {"found": False}
    return {"found": True, "device_id": device.device_id, "device_name": device.name, "rfid_uid": device.last_rfid_uid, "read_at": device.last_rfid_at.isoformat() if device.last_rfid_at else None}


@app.post("/api/transactions/sync")
def api_sync_transaction(payload: SyncTransactionPayload, db: Annotated[Session, Depends(get_db)]):
    return create_deposit_from_device(db, payload)


@app.get("/api/transactions/recent")
def api_recent_transactions(db: Annotated[Session, Depends(get_db)]):
    txs = db.query(Transaction).order_by(Transaction.created_at.desc()).limit(10).all()
    return [{
        "id": tx.id,
        "student": tx.student.name if tx.student else "",
        "classroom": tx.student.classroom.name if tx.student and tx.student.classroom else "",
        "kind": tx.kind,
        "source": tx.source,
        "weight_grams": tx.weight_grams,
        "credits": tx.credits,
        "status": tx.status,
        "created_at": tx.created_at.isoformat(),
    } for tx in txs]
