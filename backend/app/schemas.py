"""Pydantic schemas for API communication with ESP32 terminals."""

from __future__ import annotations

from pydantic import BaseModel, Field


class DeviceAuth(BaseModel):
    device_id: str = Field(min_length=1, max_length=80)
    device_key: str = Field(min_length=1, max_length=120)


class DevicePing(DeviceAuth):
    firmware_version: str | None = None
    pending_count: int = 0


class LastRfidPayload(DeviceAuth):
    rfid_uid: str = Field(min_length=1, max_length=80)


class SyncTransactionPayload(DeviceAuth):
    transaction_uid: str = Field(min_length=1, max_length=120)
    rfid_uid: str = Field(min_length=1, max_length=80)
    weight_grams: int
    credits: int
    created_at_device: str | None = None


class ApiResponse(BaseModel):
    ok: bool
    message: str
