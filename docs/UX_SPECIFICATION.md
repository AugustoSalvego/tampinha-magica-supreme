# UX Specification

## Design principle

The professor may not be technical and may use only a cellphone. Therefore, the UI is mobile-first, uses simple Portuguese, avoids technical terms, and protects dangerous actions.

## Avoid technical wording

Use:

- Cartão instead of RFID UID
- Maquininha instead of device
- Pendente de sincronização instead of unsynced payload
- Depósito instead of transaction

## Primary flows

### Register student

1. Search student first.
2. Create student with name, class and call number.
3. Link card now or later.
4. Warn about similar students before creating.

### Deposit

1. Teacher scans card with machine.
2. Machine shows student name, class and call number.
3. Teacher weighs caps.
4. Teacher confirms.
5. Machine saves locally.
6. Machine syncs online.

### Correction

1. Search student.
2. Add correction.
3. Positive or negative amount.
4. Clear reason is mandatory.
5. Teacher confirms understanding.
6. History remains immutable.

### Lost card

1. Open student details.
2. Link a new card.
3. Reason is mandatory.
4. Credits and history stay with the same student.

### Duplicate student

MVP approach:

- Warn about similar students during registration.
- Allow deactivation with strong confirmation.
- Do not delete records.

Future:

- Merge duplicate students with full audit.
