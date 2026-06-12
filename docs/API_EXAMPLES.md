# API Examples

## Health

```bash
curl http://localhost:8000/health
```

## Find student by RFID

```bash
curl http://localhost:8000/api/students/by-rfid/A1B2C3D4
```

## Sync deposit

```bash
curl -X POST http://localhost:8000/api/transactions/sync \
  -H "Content-Type: application/json" \
  -d '{
    "transaction_uid": "test-0001",
    "device_id": "terminal-01",
    "device_key": "change-this-device-key",
    "rfid_uid": "A1B2C3D4",
    "weight_grams": 142,
    "credits": 142,
    "created_at_device": "manual-test"
  }'
```

Run the same command twice to verify duplicate protection.
