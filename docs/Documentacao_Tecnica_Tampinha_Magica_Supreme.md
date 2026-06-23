# Tampinha Mágica Supreme - Documentação Técnica Completa

**Versão:** 1.0  \n**Base analisada:** Supreme MVP + firmware consolidado com PCF8574  \n**Data de referência:** 22/06/2026

## 1. Escopo, fonte e estado da documentação

Esta documentação foi produzida a partir do snapshot do repositório Tampinha Mágica Supreme e do firmware consolidado usado na integração atual de hardware. Ela separa o que está implementado no código, o que foi validado em bancada e o que ainda é uma decisão de evolução.

| Fonte | Conteúdo analisado | Observação |
| --- | --- | --- |
| Repositório Supreme MVP | README, backend FastAPI, modelos, serviços, scripts e documentos. | Base do dashboard, API e SQLite. |
| Firmware consolidado PCF | LCD 0x27, PCF8574 0x20, RFID, HX711, buzzer, LittleFS, cache e fila. | Referência operacional desta documentação. |
| Validações recentes | I2C, HX711, calibração, health e RFID online. | Persistência offline precisa de teste formal. |

> **Leitura importante:** o firmware no archive é um rascunho anterior. A versão consolidada com PCF8574 deve ser a única firmware oficial após ser copiada ao repositório e validada.

## 2. Visão do produto e regras de negócio

O Tampinha Mágica é um sistema de créditos por reciclagem para escolas. O professor é o operador e administrador do MVP. O aluno usa somente um cartão RFID e não possui login. O professor identifica o aluno, pesa as tampinhas, confirma o lançamento e acompanha o resultado pelo painel web.

| Regra | Significado técnico |
| --- | --- |
| 1 g = 1 crédito | A API rejeita credits diferente de weight_grams. |
| Professor confirma | A confirma; B cancela. |
| Sem perda | LittleFS salva antes do envio. |
| Sem duplicidade | transaction_uid garante idempotência. |
| Correção auditável | Nova Transaction com razão obrigatória. |
| Histórico preservado | Aluno é desativado, não apagado. |

## 3. Arquitetura completa

A arquitetura correta do MVP é um sistema distribuído cliente-servidor, com um monólito modular no backend e uma maquininha IoT de borda. O sistema é online-first, mas offline-safe: o banco central é a fonte oficial, enquanto o ESP32 conserva temporariamente as informações necessárias para sobreviver a falhas de rede.

```text
[Professor] -> [Dashboard Web] -> [FastAPI] -> [SQLite]
[ESP32 edge device] -> Wi-Fi/HTTP REST -> [FastAPI]
ESP32: RFID + HX711 + LCD + PCF8574 + buzzer + LittleFS (/cache, /queue)
```

| Camada | Estilo | Responsabilidade | Tecnologias |
| --- | --- | --- | --- |
| UX | Web mobile-first | Cadastro, histórico, ranking, backups. | Jinja2, HTML, sessão. |
| Aplicação | Monólito modular | Regras, idempotência, persistência. | FastAPI, Pydantic, SQLAlchemy. |
| Dados | Relacional | Fonte de verdade. | SQLite -> PostgreSQL. |
| Borda | IoT | Leitura, pesagem, LittleFS, retry. | ESP32, Arduino. |

### 3.1 Decisões arquiteturais e por que foram tomadas

| Decisão | Motivo | Trade-off |
| --- | --- | --- |
| Monólito modular | Simplicidade inicial. | Separar módulos para evolução. |
| SQLite | MVP simples. | Migrar para PostgreSQL em escala. |
| LittleFS | Recuperação simples. | Gerir capacidade local. |
| REST | Compatível com ESP32. | Fila central em escala. |
| Saldo materializado | Leitura rápida. | Reconciliar com histórico. |

## 4. Maquininha IoT e conexões físicas

A maquininha é o dispositivo de borda. Ela captura o evento físico e deve impedir que um dado inseguro chegue ao backend. O ESP32 não é a fonte oficial do saldo, mas é responsável por conservar uma transação confirmada até que o servidor a reconheça.

| Componente | Conexão/configuração | Função | Estado |
| --- | --- | --- | --- |
| LCD | GPIO21/22, 0x27 | Mensagens. | Detectado. |
| PCF8574 | GPIO21/22, 0x20, P0-P3 cols, P4-P7 rows | Teclado. | Detectado. |
| RC522 | SPI 18/19/23; SS5; RST27 | RFID. | Validado online. |
| HX711 | DOUT32, SCK33, fator -431.8 | Peso. | Calibrado em bancada. |
| Buzzer | GPIO25 | Som. | Validar fiação. |
| LittleFS | /cache, /queue | Persistência local. | Teste de reboot pendente. |

> **Regra elétrica:** todos os módulos compartilham GND. RC522 em 3.3 V. LCD em 5 V exige cuidado com pull-ups I2C, pois o ESP32 não tolera 5 V nos GPIOs.

## 5. Arquitetura interna do firmware ESP32

O firmware consolidado é organizado por responsabilidades e funciona logicamente como uma máquina de estados.

```text
BOOT -> READY -> WAIT_CARD -> LOOKUP_STUDENT -> WAIT_EMPTY_SCALE -> TARE -> WAIT_WEIGHT -> VALIDATE_STABILITY -> CONFIRM -> PERSIST_LOCAL -> SYNC_NOW -> WAIT_SCALE_EMPTY -> READY
```

### 5.1 Módulos e funções principais do firmware

| Bloco | Funções-chave | Responsabilidade |
| --- | --- | --- |
| Buzzer | beep* | Feedback. |
| LCD | showMessage | Tela limitada a 16 caracteres. |
| PCF | pcfWrite/read, getKey | Teclado I2C e debounce. |
| Wi-Fi | connect/ensure/maintain | Conexão e retry. |
| RFID | readCardUid | UID normalizado. |
| Balança | readAverage..., stableWeight... | Tara, detecção e estabilidade. |
| Cache | save/loadStudentCache | Aluno offline conhecido. |
| Fila | savePending..., sync... | Persistência e retry. |

### 5.2 Persistência local e segurança contra queda de energia

O LittleFS usa `/cache/student_<RFID>.json` para dados mínimos do aluno e `/queue/p<timestamp><random>.json` para transações pendentes.

A fila usa arquivo temporário, verifica escrita e renomeia para o caminho definitivo, reduzindo risco de persistência parcial.

## 6. Backend FastAPI: módulos, responsabilidades e arquivos

O backend é uma aplicação única organizada por responsabilidades. Ele concentra autorização da maquininha, idempotência, atualização do saldo, correções, auditoria e persistência.

| Arquivo | Papel | Pontos principais |
| --- | --- | --- |
| README.md | Entrada | Objetivo e execução. |
| requirements.txt | Dependências | Atualizar SQLAlchemy para ambiente Python 3.14. |
| app/database.py | Dados | Engine, sessão, Base e get_db. |
| app/models.py | ORM | Tabelas e relacionamentos. |
| app/schemas.py | DTO | Payloads da API do dispositivo. |
| app/security.py | Segurança | PBKDF2 e comparação segura. |
| app/services.py | Regras | Auth device, idempotência, correções. |
| app/main.py | Aplicação | Rotas web e API. |
| scripts/seed_demo.py | Seed | Dados de apresentação. |
| scripts/simulate_machine.py | Simulador | Testa terminal sem hardware. |
| tampinha_magica_consolidado_pcf.ino | Firmware atual | PCF, cache, fila, buzzer. |

## 7. Banco de dados relacional

O banco atual é SQLite e utiliza SQLAlchemy 2.0 como ORM. O histórico é a referência auditável; total_credits é um saldo materializado derivado das transações.

```text
classrooms 1---N students 1---N transactions
transactions.admin_id -> admin_users.id
devices são referenciados por device_id textual; campaigns e audit_logs são independentes no MVP.
```

### 7.1 Dicionário de tabelas

| Tabela | Campos centrais | Responsabilidade |
| --- | --- | --- |
| admin_users | username UNIQUE, password_hash | Admin. |
| classrooms | name, school_name | Turmas. |
| students | classroom_id FK, rfid_uid UNIQUE, total_credits | Aluno e saldo. |
| devices | device_id UNIQUE, device_key_hash | Maquininha. |
| transactions | transaction_uid UNIQUE, student_id FK, admin_id FK | Depósitos/correções. |
| campaigns | goal_grams | Campanhas. |
| audit_logs | actor/action/entity/details | Auditoria. |

### 7.2 Integridade, cardinalidade e restrições

| Regra | Implementação | Efeito |
| --- | --- | --- |
| Turma -> alunos | FK classroom_id | N alunos por turma. |
| Aluno -> transações | FK student_id | Histórico por aluno. |
| Admin -> correção | FK admin_id opcional | Responsável por ajuste. |
| RFID único | UNIQUE rfid_uid | Cartão exclusivo inclusive inativos. |
| UID único | UNIQUE transaction_uid | Idempotência. |

> **Falha de UX identificada:** desativar aluno não libera cartão por causa da unicidade global do RFID. Criar ação separada de desvincular cartão, com motivo e auditoria.

### 7.3 Consistência de saldo e transação

No sync, o serviço valida dispositivo, idempotência, regra 1:1 e aluno ativo; cria Transaction, atualiza total_credits, audita e faz commit.

## 8. Rotas HTTP e contratos de comunicação

As rotas se dividem em páginas web/sessão, gestão autenticada por professor e API consumida pelo ESP32. A web usa sessão; as rotas de escrita do dispositivo validam device_id e device_key.

### 8.1 Rotas de entrada e sessão

| Método e rota | Acesso | Finalidade |
| --- | --- | --- |
| GET /health | Pública | Healthcheck. |
| GET / | Pública | Redirecionamento. |
| GET /login | Pública | Login. |
| POST /login | Pública | Cria sessão. |
| GET /logout | Sessão | Limpa sessão. |
| GET /dashboard | Admin | Métricas. |

### 8.2 Rotas de gestão web

| Método e rota | Acesso | Efeito |
| --- | --- | --- |
| GET /students | Professor | Lista e filtra. |
| POST /students/create | Professor | Cria; RFID exclusivo. |
| POST /students/{id}/link-card | Professor | Vincula/troca com razão. |
| POST /students/{id}/deactivate | Professor | Soft delete protegido. |
| POST /corrections/create | Professor | Correção auditável. |
| GET /transactions/export.csv | Professor | CSV. |
| GET /devices | Professor | Dispositivos. |
| POST /backups/create | Professor | Backup SQLite. |

### 8.3 API da maquininha ESP32

| Método e rota | Autenticação | Efeito |
| --- | --- | --- |
| GET /api/students/by-rfid/{uid} | Nenhuma | Busca aluno ativo. |
| POST /api/devices/ping | Device auth | Atualiza presença. |
| POST /api/devices/last-rfid | Device auth | Telemetria RFID. |
| GET /api/devices/last-rfid | Nenhuma | Último RFID. |
| POST /api/transactions/sync | Device auth | Sync idempotente. |
| GET /api/transactions/recent | Nenhuma | Últimas 10. |

### 8.4 Exemplo de sincronização de depósito

```json
{
  "transaction_uid": "terminal-01-<timestamp>-<random>",
  "device_id": "terminal-01",
  "device_key": "<segredo>",
  "rfid_uid": "<uid>",
  "weight_grams": 64,
  "credits": 64
}
```

| Condição | Resposta |
| --- | --- |
| Novo UID | 200, duplicated:false. |
| Mesmo UID | 200, duplicated:true. |
| Peso inválido | 400. |
| Device inválido | 401. |
| Aluno não ativo | 404. |

## 9. Processos críticos ponta a ponta

### 9.1 Inicialização da maquininha

- Inicializa Serial, buzzer, I2C/PCF e LCD.
- Monta LittleFS e diretórios.
- Inicializa SPI/RC522/HX711 e realiza tara.
- Conecta Wi-Fi, envia ping e sincroniza pendências.
- Entra em espera de cartão.

### 9.2 Depósito online

- RFID -> lookup online -> cache local.
- Balança vazia -> tara -> peso acima de 3 g -> estabilidade.
- A confirma / B cancela.
- Grava em LittleFS antes de POST.
- HTTP 2xx remove pendência; falha mantém arquivo.

### 9.3 Depósito offline e consistência eventual

Quando a rede cai, o terminal usa cache de cartões já conhecidos e mantém a transação em /queue. O painel pode ficar temporariamente desatualizado, mas o dado sincroniza depois: consistência eventual.

### 9.4 Idempotência e cenário de resposta perdida

```text
ESP32 envia X -> servidor grava X -> resposta se perde -> ESP32 reenvia X -> servidor retorna duplicated:true -> um único crédito.
```

### 9.5 Correção administrativa

- Motivo obrigatório e confirmação.
- Cria nova Transaction CORRECTION.
- Não permite valor zero ou saldo negativo.
- Histórico original permanece.

### 9.6 Cartão perdido, reuso e desativação

Link-card troca ou vincula RFID com motivo. Desativação preserva histórico, mas não libera RFID; criar ação separada de Desvincular cartão com motivo e auditoria.

## 10. Segurança, confiabilidade e limites atuais

### 10.1 O que já está protegido

| Mecanismo | Implementação | Valor |
| --- | --- | --- |
| Senha | PBKDF2 + salt | Sem texto puro. |
| Device key | Hash no backend | Protege segredo em repouso. |
| Idempotência | UID único | Sem duplicidade. |
| Auditoria | AuditLog | Rastreabilidade. |
| LittleFS | Grava antes de enviar | Resiliência. |

### 10.2 Riscos e melhorias obrigatórias antes de produção

| Tema | MVP | Melhoria |
| --- | --- | --- |
| Segredos | Possível hardcode no .ino. | secrets.h ignorado no Git. |
| HTTP | Sem TLS. | HTTPS. |
| API aberta | Algumas leituras sem auth. | Auth/rate limit. |
| SQLite | MVP local. | PostgreSQL. |
| Cache | Somente já consultados. | Sync completo. |
| Tara | Warm-up precisa reforço. | wait_ready + leituras iniciais. |
| Device FK | String textual. | FK formal. |

## 11. Operação local, execução e diagnóstico

### 11.1 Subir o backend no Windows

```powershell
cd C:\Users\<usuario>\Documents\tampinha-magica-supreme\backend
.\.venv\Scripts\Activate.ps1
python -m uvicorn app.main:app --host 0.0.0.0 --port 8000
```

A API responde em `http://127.0.0.1:8000/health` no PC. O ESP32 deve usar o IPv4 LAN do PC, nunca 127.0.0.1.

```powershell
ipconfig
```
Depois acessar `http://<ipv4-do-pc>:8000/health`.

### 11.2 Configuração de firmware

```cpp
const char* WIFI_SSID = "<rede-2.4-ghz>";
const char* WIFI_PASSWORD = "<segredo>";
const char* API_BASE_URL = "http://<ipv4-do-pc>:8000";
const char* DEVICE_ID = "terminal-01";
const char* DEVICE_KEY = "<segredo-do-terminal>";
```

- ESP32 requer Wi-Fi 2.4 GHz.
- Uvicorn deve usar 0.0.0.0 e Firewall liberar TCP 8000 Private.
- Teste /health em celular do mesmo Wi-Fi.
- Não comitar segredos ou IPs internos.

### 11.3 Roteiro de validação recomendado

- Health OK.
- Cadastro + lookup por RFID.
- I2C 0x20/0x27 + HX711 READY.
- Teclas A/B.
- Coleta online.
- Reenvio do mesmo UID.
- Offline, reboot e reconexão.
- Auditoria.

## 12. Estado atual do projeto e próximos passos técnicos

| Área | Situação | Próximo passo |
| --- | --- | --- |
| Backend | /health validado; ambiente usa SQLAlchemy atualizado. | Atualizar requirements e migrations. |
| RFID/API | Reconhecimento online validado. | Vínculo guiado. |
| I2C | 0x20 e 0x27 detectados. | Testar teclas. |
| HX711 | Pinos e calibração corrigidos. | Warm-up/tara e repetição. |
| Fila | Código robusto. | Testes offline/reboot. |
| Cartão | Desativação não libera RFID. | Unlink auditável. |

### 12.1 Ordem recomendada de evolução

- Consolidar firmware oficial e segredos fora do Git.
- Validar tara, teclado e coleta online.
- Testar offline/reboot/idempotência.
- Adicionar unlink RFID e histórico de cartões.
- Migrations, testes e PostgreSQL.
- HTTPS, env vars, auth de leitura e backup automatizado.

## 13. Como explicar o projeto tecnicamente

## Como explicar o projeto

> O Tampinha Mágica é um sistema distribuído para créditos de reciclagem em escolas. A maquininha ESP32 funciona como edge device: lê RFID, valida pesagem, pede confirmação do professor e grava a transação localmente antes de sincronizar. O backend é um monólito modular em FastAPI com API REST, regras de negócio, SQLite no MVP, auditoria e proteção contra duplicidade por transaction_uid. Ele é online-first, mas offline-safe: se a internet falha, a transação fica persistida no LittleFS e é reenviada depois sem duplicar crédito.


## 14. Glossário

| Termo | Definição |
| --- | --- |
| Edge device | ESP32 perto do evento físico. |
| Online-first | Servidor é fonte oficial. |
| Offline-safe | Sem perda de coleta confirmada. |
| Idempotência | Mesmo UID não duplica. |
| Consistência eventual | Dashboard pode atrasar e sincroniza depois. |
| Saldo materializado | total_credits derivado do histórico. |
| Audit trail | Rastro de ações. |
| Soft delete | Desativar sem apagar. |
| Tara | Zero da balança. |

> **Conclusão:** o núcleo arquitetural já é coerente para um MVP sério. O próximo marco é consolidar firmware, concluir testes de pesagem/offline e eliminar lacunas de segurança e ciclo de cartão.
