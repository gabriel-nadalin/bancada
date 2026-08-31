# DAHDI / Wanpipe E1 Test Bench

Servidor: `bancada-dialup`
Data: 2026-05-09

## Objetivo

Configurar a placa Sangoma PCI E1 da bancada para conectar em um RAS Cisco AS5300 usando DAHDI/Wanpipe, com E1 CCS/HDB3/CRC4 e canal D no timeslot 16.

A bancada será usada para testes com modems/V.90, portanto a configuração deve evitar cancelamento de eco nos canais B.

## Hardware Detectado

Placa PCI detectada:

```text
04:02.0 Network controller [0280]: Sangoma Technologies Corp. A200/Remora FXO/FXS Analog AFT card [1923:0040]
```

O `dahdi_hardware` reportou:

```text
pci:0000:04:02.0     wanpipe-     1923:0040 Sangoma Technologies Corp. A200/Remora FXO/FXS Analog AFT card
```

Durante o carregamento, o driver identificou a placa como E1/T1 Sangoma AFT:

```text
wanpipe: AFT-A101-SH PCI T1/E1 card found (HDLC (DS) rev.34), cpu(s) 1, bus #4, slot #2
```

## Pacotes Instalados

Pacotes relevantes instalados:

```text
dahdi-tools 3.4.0-2
dahdi-linux-git 3.4.0.rc1.r20.gd1c842a-2
wanpipe 7.0.38-6
```

## Configuração Wanpipe

Arquivo: `/etc/wanpipe/wanpipe1.conf`

```ini
[devices]
wanpipe1 = WAN_AFT_TE1, Cisco AS5300 E1

[interfaces]
w1g1 = wanpipe1, , TDM_VOICE, Cisco AS5300 E1

[wanpipe1]
CARD_TYPE = AFT
S514CPU = A
CommPort = PRI
AUTO_PCISLOT = NO
PCISLOT = 2
PCIBUS = 4
FE_MEDIA = E1
FE_LCODE = HDB3
FE_FRAME = CRC4
FE_LINE = 1
TE_CLOCK = NORMAL
TE_REF_CLOCK = 0
TE_SIG_MODE = CCS
TE_HIGHIMPEDANCE = NO
LBO = 120OH
FE_TXTRISTATE = NO
MTU = 1500
UDPPORT = 9000
TTL = 255
IGNORE_FRONT_END = NO
TDMV_SPAN = 1
TDMV_DCHAN = 16
TDMV_HW_DTMF = NO
TDMV_HW_FAX_DETECT = NO

[w1g1]
ACTIVE_CH = ALL
TDMV_ECHO_OFF = YES
```

Notas:

- `PCIBUS = 4` e `PCISLOT = 2` correspondem ao barramento PCI detectado.
- `TE_CLOCK = NORMAL` faz a Sangoma recuperar clock do E1; o Cisco AS5300 é o master.
- `FE_FRAME = CRC4` corresponde a E1 CCS com CRC4.
- `TDMV_DCHAN = 16` define o timeslot 16 como canal D.
- `TDMV_ECHO_OFF = YES` desliga o controle de eco no lado Wanpipe para a interface.

## Configuração DAHDI

Arquivo: `/etc/dahdi/system.conf`

```ini
span=1,1,0,ccs,hdb3,crc4
bchan=1-15,17-31
hardhdlc=16
loadzone=br
defaultzone=br
```

Notas:

- `span=1,1,0,ccs,hdb3,crc4`: span 1, clock vindo do remoto, E1 CCS/HDB3/CRC4.
- `bchan=1-15,17-31`: canais B.
- `hardhdlc=16`: canal D ISDN PRI com HDLC/FCS assistido pelo driver Sangoma/Wanpipe.
- Não há `echocanceller` configurado; o `dahdi_cfg -vv` deve reportar `Echo Canceler: none` nos canais.

## Comandos Para Subir o Link

Sequência usada:

```sh
sudo wanrouter stop || true
sudo modprobe -r wanpipe wan_aften wanec wanrouter sdladrv dahdi 2>/dev/null || true
sudo modprobe dahdi
sudo wanrouter start
sudo dahdi_cfg -vv
```

Resultado esperado:

- `wanrouter start` deve carregar `wanpipe` e iniciar `wanpipe1`.
- `dahdi_cfg -vv` deve aplicar o span e canais sem erro.

## Estado Atual Verificado

### DAHDI Scan

Comando:

```sh
sudo dahdi_scan
```

Saída relevante:

```text
[1]
active=yes
alarms=OK
description=wanpipe1 card 0
name=WPE1/0
manufacturer=Sangoma Technologies
devicetype=A101
location=SLOT=2, BUS=4
basechan=1
totchans=31
type=digital-E1
coding=HDB3
framing=CCS/CRC4
```

### /proc/dahdi/1

Comando:

```sh
sudo sed -n '1,120p' /proc/dahdi/1
```

Saída relevante:

```text
Span 1: WPE1/0 "wanpipe1 card 0" (MASTER) CCS/HDB3/CRC4

   1 WPE1/0/1 Clear
   2 WPE1/0/2 Clear
   ...
  15 WPE1/0/15 Clear
  16 WPE1/0/16 Hardware assisted D-channel
  17 WPE1/0/17 Clear
   ...
  31 WPE1/0/31 Clear
```

### Kernel Log

Comando:

```sh
sudo dmesg | grep -iE 'wanpipe|WPE1|red alarm|yellow alarm|blue alarm|alarm|los|crc|dahdi' | tail -n 100
```

Saída relevante:

```text
wanpipe1: Wanpipe device is registered to Zaptel span # 1!
wanpipe1: RED : OFF
wanpipe1: LOF alarm is 2 OFF
wanpipe1: E1 connected!
wanpipe1: AFT communications enabled
```

## Resultado Atual

Layer 1 E1: OK

DAHDI span/canais: OK

Clock:

- Wanpipe configurado com `TE_CLOCK = NORMAL`, recuperando clock da linha E1.
- DAHDI configurado com `span=1,1,0,ccs,hdb3,crc4`, usando o remoto como fonte preferida de clock.
- Contadores de manutenção sem erros de framing, CRC, code violations ou slips observados no Cisco.

Configuração atual:

- E1 CCS/HDB3/CRC4
- Canal D no timeslot 16 com `hardhdlc`
- Canais B em 1-15 e 17-31
- Sem echo canceller anexado pelo DAHDI
- Echo control desligado no Wanpipe (`TDMV_ECHO_OFF = YES`)

## ISDN / Q.931

Foi criado um utilitário mínimo de teste em `/usr/local/bin/pri-call-test` usando DAHDI + libpri.

Arquivos fonte no servidor:

```text
/home/matias/pri-call-test/pri-call-test.c
/home/matias/pri-call-test/Makefile
```

Uso básico:

```sh
sudo pri-call-test -t 3 -b 2 -
```

Manter apenas Q.921 ativo, sem originar chamada:

```sh
sudo pri-call-test -k
```

Estado validado:

- `libpri 1.6.1-3` instalado.
- O utilitário abre o D-channel DAHDI 16 via `/dev/dahdi/channel`.
- Q.921 sobe com `PRI_EVENT_DCHAN_UP`.
- O utilitário envia `SETUP` como CPE/TE EuroISDN E1.
- O Cisco AS5300 atua como network side; rodar o utilitário como `network` reporta erro de configuração porque os dois lados tentam ser network.
- No AS5300, `Serial1:15` fica em `MULTIPLE_FRAME_ESTABLISHED` enquanto o utilitário mantém o D-channel aberto.

Resultado atual da chamada de teste:

- Com B-channel 2 explícito e número chamado vazio, o AS5300 responde `CALL_PROCEEDING`, `ALERTING` e `CONNECT`.
- A chamada conectada foi encerrada pelo utilitário após o tempo configurado com `-t`.
- O B-channel selecionado no teste foi o canal 2.

Próximo passo:

- abrir o B-channel selecionado após `CONNECT`;
- acoplar a biblioteca userspace de softmodem para troca de amostras de áudio.
