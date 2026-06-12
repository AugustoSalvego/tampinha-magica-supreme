# Test Plan

## Notebook tests without hardware

1. Start backend.
2. Seed demo data.
3. Login with admin/admin123.
4. Search students.
5. Open student details.
6. Create classroom.
7. Create student with call number.
8. Try duplicate/similar student.
9. Simulate machine deposit.
10. Repeat same transaction UID manually to verify no duplication.
11. Add positive correction with reason.
12. Add negative correction with reason.
13. Try correction without reason and verify rejection.
14. Try deactivation without typing DESATIVAR and verify rejection.
15. Generate backup.
16. Export CSV.

## Hardware tests later

- RFID UID reading.
- LCD I2C address.
- Keypad mapping.
- HX711 calibration.
- Weight stability.
- Offline save.
- Offline retry.
- Power loss during pending transaction.
- Server restart while ESP32 has pending transactions.
