# Tampinha Mágica Supreme - Project Master

## Product vision

Tampinha Mágica is a professional school recycling credit system. Students receive credits based on the weight of plastic caps delivered to the teacher.

Main rule:

```text
1 gram = 1 credit
```

## MVP decision

- The teacher is also the administrator.
- The teacher may work with many classes and even multiple schools.
- The student does not access the MVP system.
- The student only has an RFID card.
- The machine stays with the teacher.
- The teacher confirms each deposit.
- The system is online-first.
- The terminal must remain offline-safe.
- No confirmed credit can be lost.

## Student registration

Keep registration simple:

- Student name
- Classroom
- Call number
- RFID card
- Optional notes only when needed

The call number is used in the MVP to help distinguish students with the same name.

## Data integrity rule

When the teacher confirms a deposit:

1. The ESP32 generates a unique transaction UID.
2. The ESP32 saves the transaction locally first.
3. The transaction is marked as pending locally.
4. The ESP32 tries to send it to the server.
5. The server stores it idempotently.
6. The server confirms success.
7. The ESP32 marks it as synced.

Never delete a local transaction before server confirmation.

## Correction rule

Credits are never edited directly. Corrections can be positive or negative, but must always include a clear mandatory reason.

## Dangerous action rule

Dangerous actions such as deactivating a student must never be easy to click accidentally. They require a protected confirmation flow.

## UI language

- Code/comments/docs: English.
- User interface: Brazilian Portuguese.
