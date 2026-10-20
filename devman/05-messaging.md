<!-- DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. -->
# 5. Messaging

Messaging is the centre of DOORS, as it was of MMURTL. Its code is in `kernel/msg.c` and
`kernel/rqb.c`, and its declarations in `include/doors/msg.h`.

## Exchanges

An **exchange** is a mailbox, identified by a number. It holds either waiting messages or waiting
tasks, never both.

```c
U32 exch;
AllocExch(&exch);          /* a new exchange, owned by the caller's job */
...
DeAllocExch(exch);         /* messages still queued on it are freed */
```

Every task also has an exchange of its own (`GetTSSExch`). `Sleep` uses it, so do not use a task's
own exchange for other messages while it sleeps.

## Messages

A message is **two `U64` values**. MMURTL's messages were two 32-bit values.

```c
SendMsg(exch, 0x1234, 0x5678);   /* queue a message; wakes a task waiting there */

U64 m[2];
WaitMsg(exch, m);                /* block until a message arrives */
if (CheckMsg(exch, m) == ErcNoMsg) { /* nothing there; returns at once */ }
```

- Messages are delivered in FIFO order.
- `SendMsg` to an exchange where a task waits makes that task ready. If its priority is higher than
  the sender's, it runs immediately.
- Interrupt handlers use `ISendMsg` (kernel only). It only makes the waiting task ready; it never
  switches tasks itself. The switch happens at the next scheduling point (for the timer IRQ, at
  the end of the interrupt).

## Timers

```c
Sleep(10);                 /* block for 10 ticks = 100 ms */
Alarm(exch, 50);           /* after 50 ticks, send a timer message to exch */
KillAlarm(exch);           /* cancel pending alarms for exch */
GetTimerTick(&t);          /* ticks since boot */
```

A timer message has both halves set to `TIMER_MSG` (`0xFFFFFFFFFFFFFFFF`).

## Services

A **service** is a task that registers an exchange under an 8-character name. Pad the name with
spaces.

```c
AllocExch(&exch);
RegisterSvc("UPCASE  ", exch);
...
UnRegisterSvc("UPCASE  ");
```

The file system (`"FILESYS "`) is such a service. The self-tests also use `"KECHO   "` and
`"NUMBERS "`.

The kernel's **`"KEYBOARD"`** service is the request interface to the keyboard that MMURTL
programs used. It is useful when a program wants a key and other messages on the **same**
exchange (MMURTL's Service sample waits for a client or a key that way):

| Code | Request |
|---|---|
| 1 | read a key: `pData1` receives the 4-byte key code (`npSend` 0). `dData0` = 1 waits until a key is typed on the requester's screen; 0 answers `ErcNoKeyAvail` at once if there is none. One waiting request per job |
| 4 | give the keyboard to job `dData0`. Accepted, but has no effect: in DOORS the keyboard always follows the screen (`SetVidOwner`) |

```c
U32 key;
U64 kbd;

Request("KEYBOARD", 1, exch, &kbd, 0, (U8 *)&key, 4, 0, 0, 1, 0, 0);
WaitMsg(exch, m);
if (m[0] == kbd)        /* the key arrived (not a client request) */
	...
```

## Request and Respond

A client calls a service with `Request`:

```c
erc_t Request(const char *pSvcName, U32 wSvcCode, U32 dRespExch, U64 *pRqHndlRet,
              U32 dnpSend, U8 *pData1, U64 dcbData1, U8 *pData2, U64 dcbData2,
              U32 dData0, U32 dData1, U32 dData2);
```

| Parameter | Meaning |
|---|---|
| `pSvcName` | 8-character service name |
| `wSvcCode` | what to do (service-defined). Code 0 is reserved: it tells the service a requester is going away |
| `dRespExch` | exchange where the response will arrive |
| `pRqHndlRet` | receives the request handle |
| `dnpSend` | how many of the two buffers the service only reads (0, 1 or 2). The others are receive buffers, which the service writes |
| `pData1`, `dcbData1`, `pData2`, `dcbData2` | up to two buffers in the client's memory |
| `dData0..2` | three 32-bit values for the service (no pointers) |

The service receives a message whose first half is the **request handle**: a pointer to a
`struct RqBlk` (`include/doors/rqblk.h`). In it, `pData1` and `pData2` are already **aliased**
into the service's address space, so the service reads and writes the client's buffers directly.
When the service is done:

```c
Respond(handle, ErcOk);    /* status goes back to the client */
```

The client receives a message on `dRespExch`: first half = the request handle, second half = the
status the service returned. The aliases are removed at that point.

Rules:

- From ring 3, `Request` refuses buffers outside the user half with `ErcBadLinAdd`.
- A **receive buffer** (index ≥ `dnpSend`) must be writable memory of the client. This is checked
  when the service receives the request: the service's `WaitMsg` then returns `ErcBadLinAdd`
  with the request handle still in the first half of the message, and nothing is aliased. The
  service must answer with `Respond(handle, erc)` so the client gets the error, as FILESYS does
  (`kernel/fsys.c`). `ErcNoMem` (no room for the aliases) is handled the same way.
- A user-mode service sees request blocks through a **read-only window** (`RQB_UBASE`). It may read
  the request block but not change it.
- `MoveRequest(handle, exch)` passes a request on to another exchange, for example to a worker task.
- If the client's job ends while its request is in progress, the request becomes an *orphan*: the
  buffers are dropped, and the service's eventual `Respond` is accepted and discarded.

## Example: a client

```c
U32 exch;
U64 h, m[2];
char out[64];

GetTSSExch(&exch);
if (Request("UPCASE  ", 1, exch, &h, 1, (U8 *)"doors", 6, (U8 *)out, sizeof(out), 0, 0, 0) == ErcOk &&
    WaitMsg(exch, m) == ErcOk && m[0] == h && m[1] == ErcOk)
	printf("%s\n", out);       /* DOORS */
```

The matching service is in [chapter 7](07-programming.md#writing-a-service).

## Limits

| Object | Count |
|---|---|
| exchanges | grows as needed, up to 65,536 |
| link blocks (queued messages) | grows as needed, up to 1,048,576 |
| request blocks | grows as needed, up to 65,536 |
| named services | 64 |
| pending timers | 64 |

A message to an exchange where a task is waiting goes straight to that task and needs no link
block; only queued messages use them. So a timer or a reply still wakes its task when the link blocks
are used up.

When a pool is empty (a growing one: at its ceiling or out of memory), the call returns `ErcNoMoreExch`, `ErcNoMoreLBs`, `ErcNoMoreRqBlks`,
`ErcOutOfSvcDesc` or `ErcNoMoreTBs`. The monitor's `tasks` command shows the free counts.
