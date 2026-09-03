#set page(margin: 2.2cm)
#set text(font: "Libertinus Serif", size: 10.5pt, lang: "pt")
#set par(justify: true)
#set heading(numbering: "1.")
#set page(numbering: "1", footer: context {
  let p = counter(page).get().first()
  if p > 1 { counter(page).display("1") }
})

// ============================================================
// CAPA (preencher os campos marcados antes da submissão)
// ============================================================
#align(center)[
  #text(size: 17pt, weight: "bold")[
    Bancada de testes para engenharia reversa do softmodem slmodem:
    validação de V.90 em E1 e SIP/RTP
  ]
  #v(1em)
  #text(size: 12pt, style: "italic")[Relatório técnico-científico]
  #v(2em)
  #text(size: 11pt)[
    *Autor:* [Preencher: nome do autor] \
    *Orientador(a):* [Preencher: nome do orientador] \
    *Instituição / Departamento:* [Preencher: instituição, curso] \
    *Data:* Setembro de 2026
  ]
]

#pagebreak()

// ============================================================
// RESUMO / ABSTRACT
// ============================================================
#align(center)[
  #text(size: 13pt, weight: "bold")[Resumo]
]
#v(0.4em)

O softmodem slmodem implementa um modem de discagem em espaço de usuário,
mas seu núcleo de processamento digital de sinais, o objeto binário
`dsplibs.o` (x86-32), nunca foi disponibilizado em código-fonte. Este trabalho
descreve a construção e a validação de uma bancada de testes para a engenharia
reversa incremental desse núcleo, com foco nos protocolos V.34 e V.90. Como o
V.90 exige terminação digital E1/T1 no servidor, a bancada integra um servidor
de acesso remoto Cisco AS5300 com modems digitais MICA e um gateway de voz
Cisco 2911, além de um hardmodem de referência, um adaptador VoIP (ATA)
Grandstream HT503 e uma interface E1 Sangoma. Um orquestrador Python
(`dialbench`) conecta o slmodem aos transportes SIP/RTP e E1 e valida chamadas
de ponta a ponta pelo caminho de dados. Os resultados mostram conectividade de
V.21 a V.90 no E1 direto e no SIP/RTP direto para o gateway; o caminho que
inclui o ATA HT503 atinge V.34, mas não completa a convergência V.90. A
análise espectral das capturas e as simulações do receptor reconstruído
localizam o fator limitante na conversão analógica (D/A--A/D) e nos relógios
assíncronos do ATA, não no jitter de pacotes RTP. Adicionalmente, define-se um
procedimento de medição de latência do caminho de mídia para comparar
diferentes empilhamentos RTP/SIP sob a mesma extremidade distante.

#v(0.6em)
*Palavras-chave:* engenharia reversa; modem V.90; softmodem; slmodem;
sinalização E1/PRI; VoIP/SIP.

#pagebreak()

#text(size: 11pt)[
  *Abstract.* The slmodem softmodem runs a dial-up modem in user space, yet its
  core digital-signal-processing unit, the x86-32 binary object `dsplibs.o`,
  was never released as source code. This work describes the construction and
  validation of a test bench for the incremental reverse engineering of that
  core, targeting the V.34 and V.90 protocols. Because V.90 requires digital
  E1/T1 termination at the server, the bench integrates a Cisco AS5300 remote
  access server with MICA digital modems and a Cisco 2911 voice gateway, plus
  a reference hardmodem, a Grandstream HT503 VoIP analog terminal adapter
  (ATA), and a Sangoma E1 interface. A Python orchestrator (`dialbench`)
  connects the slmodem to the SIP/RTP and E1 transports and validates calls
  end-to-end through the data path. Results show connectivity from V.21 to
  V.90 on both direct E1 and direct SIP/RTP toward the gateway; the path that
  includes the HT503 ATA reaches V.34 but does not complete V.90 convergence.
  Spectral analysis of the captures and simulations of the reconstructed
  receiver localize the limiting factor in the ATA's analog (D/A--A/D)
  conversion and asynchronous clocks, rather than in RTP packet jitter.
  Additionally, a media-path latency measurement procedure is defined to
  compare distinct RTP/SIP stacks under the same far end.

  *Keywords:* reverse engineering; V.90 modem; softmodem; slmodem; E1/PRI
  signaling; VoIP/SIP.
]

#pagebreak()

#outline(title: "Sumário")
#pagebreak()

// ============================================================
// 1. INTRODUÇÃO
// ============================================================
= Introdução

== Contexto e motivação

O slmodem é um driver de softmodem licenciado sob BSD, originalmente
desenvolvido pela Smart Link Ltd., que implementa o modem em espaço de
usuário por meio de uma interface serial virtual (comandos AT e dados) e de
uma interface de áudio para o processamento de sinais. Seu componente central,
`dsplibs.o`, permanece distribuído como objeto binário x86-32 sem
código-fonte, o que torna qualquer esforço de reconstrução ou de entendimento
do comportamento do modem uma atividade de engenharia reversa [12].

A reconstrução incremental desse núcleo exige dois níveis complementares de
evidência: (i) testes unitários contra entradas e saídas de referência e
(ii) testes de integração que estabeleçam chamadas reais contra um servidor de
acesso. O segundo nível é indispensável para os protocolos de alta
velocidade, em particular o V.90, cuja operação de recepção em até 56.000
bit/s pressupõe uma conexão digital na extremidade do servidor (E1/T1) e
modems digitais nesse lado [10]. Essa condição não é reproduzível com modems
convencionais ou apenas com simulação em software, o que justifica a montagem
de uma bancada física.

== Problema

Para validar a reconstrução do DSP, é preciso exercitar o slmodem contra um
RAS real, em transportes distintos, e obter um critério objetivo de aprovação
que não confunda um carrier isolado com uma conexão de dados funcional.
Observa-se, além disso, que o desempenho do protocolo varia conforme o caminho
de mídia empregado: o trecho que atravessa a conversão analógica de um ATA
apresenta limitação específica. Este relatório documenta a infraestrutura
construída e a evidência coletada para caracterizar essa limitação.

== Objetivos

_Objetivo geral._ Projetar, montar e validar uma bancada de testes para a
engenharia reversa do núcleo DSP do slmodem, capaz de exercitar os protocolos
V.21, V.22, V.22bis, V.32bis, V.34 e V.90 sobre transportes E1 e SIP/RTP.

_Objetivos específicos:_
- estabelecer chamadas reais de V.21 a V.90 contra um RAS Cisco AS5300/MICA
  por E1 direto e por SIP/RTP, coletando taxa negociada, eventos de
  treinamento, erros e tráfego de dados;
- definir um critério de aprovação end-to-end que exija tráfego de dados
  completo pelo caminho do modem (não apenas carrier);
- isolar experimentalmente o efeito da conversão analógica de um ATA sobre a
  convergência do V.90;
- definir um procedimento de medição de latência do caminho de mídia para
  comparar empilhamentos RTP/SIP distintos.

== Contribuições

O repositório versionado agrega: pontes userspace entre o slmodem e os
transportes SIP/RTP e E1; uma ferramenta de sinalização PRI; um orquestrador
Python (`dialbench`) que administra chamadas e valida respostas end-to-end
pela PTY do modem; e registros de resultados por topologia em `tests/`. A
evidência end-to-end valida a capacidade do conjunto para V.90 no E1 direto e
no SIP/RTP direto e localiza uma limitação específica no caminho que inclui o
ATA HT503.

== Organização do relatório

A Seção 2 apresenta a fundamentação e o contexto técnico. A Seção 3 descreve
materiais e métodos: equipamentos, arquitetura de software, topologias,
procedimento de validação de chamadas e medição de latência. A Seção 4 reúne
os resultados e a Seção 5 discute a evidência e suas limitações. A Seção 6
conclui e indica trabalhos futuros. O Apêndice A consolida a reprodutibilidade
do build.

// ============================================================
// 2. FUNDAMENTAÇÃO
// ============================================================
= Contexto técnico e fundamentação

O projeto de pesquisa original, documentado em #link("projeto.pdf")[1],
propõe a bancada com o objetivo central de validar a reconstrução de
`dsplibs.o` por meio de conexões reais, sobretudo V.90 [1, 2]. Não há, até o
momento, implementação em software livre de modems além de V.22 que esteja
completa para V.34/V.90 [12]; a opção adotada é, portanto, exercitar o
slmodem contra servidores de acesso reais com terminação digital.

O V.90 combina um enlace ascendente QAM (até 33.600 bit/s) com um enlace
descendente PCM (até 56.000 bit/s), valendo-se do fato de a extremidade do
servidor estar conectada digitalmente à rede telefônica [10]. O servidor de
acesso empregado, Cisco AS5300 com modems digitais MICA, atende a essa
condição por meio de troncos E1 com sinalização PRI [2]. Em paralelo, o
gateway Cisco 2911 faz a interligação entre o domínio analógico (portas FXS)
e o E1 PRI; o adaptador VoIP HT503 converte o áudio digital do SIP/RTP em
sinal analógico na porta FXS.

Os componentes e papéis previstos no projeto são detalhados na Seção 3.1. Por
se tratar de um primeiro relatório de engenharia, a fundamentação aqui é
restrita ao que é observável nos artefatos versionados; configuração de
laboratório e fontes externas ao repositório são tratadas como dependências de
reprodução (Apêndice A). Referências acadêmicas adicionais sobre V.90,
V.34 e sobre a plataforma slmodem estão previstas e devem ser completadas
antes da submissão final (entradas [10]--[12] da Bibliografia).

// ============================================================
// 3. MATERIAIS E MÉTODOS
// ============================================================
= Materiais e métodos

== Descrição da bancada e equipamentos

A bancada integra os equipamentos da Tabela 1. O AS5300 é o servidor de
acesso que termina as chamadas com modems digitais MICA sobre E1 PRI; o Cisco
2911 é o gateway de voz que faz a ponte entre portas FXS analógicas e o
tronco E1 (VWIC3-1MFT-T1/E1, VIC3-4FXS e PVDM3); o hardmodem USB Lenovo
(Conexant CX93010) é o modem de referência de hardware; o ATA Grandstream
HT503 converte VoIP em analógico; e o computador Linux hospeda o slmodem, as
ferramentas de coleta e os testes automatizados [2, 4].

#figure(
  table(
    columns: (4.6cm, 5.0cm, 5.6cm),
    inset: 5pt,
    align: left,
    table.header[Equipamento][Função][Observação],
    [Cisco AS5300], [RAS: termina chamadas de discagem], [Modems digitais MICA; E1 PRI; firmware `mica-modem-pw.2.9.5.0.bin`; V.90 por terminação digital.],
    [Cisco 2911], [Gateway de voz FXS \(harr\) E1], [VWIC3-1MFT-T1/E1, VIC3-4FXS, PVDM3; PRI `timeslots 1-10,16`.],
    [Hardmodem USB Lenovo (Conexant CX93010)], [Cliente de referência], [Elimina variáveis de software na validação inicial.],
    [ATA VoIP Grandstream HT503], [Conversão VoIP \(harr\) analógico], [1\(times\)FXS + 1\(times\)FXO; introduz o domínio analógico no caminho SIP.],
    [Interface E1 Sangoma], [Conexão digital direta do slmodem ao AS5300], [Caminho E1 sem gateway nem domínio analógico.],
    [Computador Linux], [Hospeda slmodem, pontes e orquestração], [Repositório versionado em Git.],
  ),
  caption: [Equipamentos da bancada e seus papéis.]
)

== Arquitetura de software

A solução é organizada em camadas de software (Tabela 2): a infraestrutura de
rede e o agente SIP são fornecidos pelas árvores `re` e `baresip`; o softmodem
slmodem implementa controle AT, temporização, empacotamento e o DSP legado;
as pontes em C convertem áudio PCM, SIP/RTP e canais B PRI; e o orquestrador
`dialbench` inicia processos, conecta fluxos PCM e valida respostas
end-to-end pela PTY do modem [9].

#figure(
  table(
    columns: (2.8cm, 4.2cm, 8.2cm),
    inset: 5pt,
    align: left,
    table.header[Camada][Componente][Responsabilidade],
    [Infraestrutura RTP/SIP], [`re`], [Loop assíncrono, sockets, SIP, SDP, RTP/RTCP, codecs e transporte de rede.],
    [Agente SIP], [`baresip`], [User-Agent modular e módulos de áudio, codecs, controle e rede.],
    [Softmodem], [`slmodem`], [Controle AT, temporização, empacotamento e DSP legado; `dsplibs.o` é dependência binária x86-32.],
    [Pontes C], [`tools/`], [Converte áudio PCM, SIP/RTP e canais B PRI. `slmodem_bridge` expõe PTY e PCM; `rtp_bridge` negocia PCMA; `pri_call` origina Q.931 e faz a ponte A-law.],
    [Orquestração], [`dialbench`], [Inicia processos, conecta fluxos PCM, administra chamadas e valida respostas end-to-end pela PTY.],
  ),
  caption: [Camadas de software da solução.]
)

O fluxo de dados comum é uma cadeia bidirecional de amostras de áudio. O
`slmodem_bridge` encapsula o DSP e apresenta PCM linear; a partir daí, o áudio
segue por RTP/PCMA até um gateway SIP ou por A-law em um canal B PRI. Em
paralelo, a PTY transporta comandos AT durante o estabelecimento e dados
seriais após o `CONNECT`.

O `Makefile` da raiz delega a compilação para `tools/`. O build das pontes
depende de três árvores integradas (`re`, `baresip` e `slmodem`); a ponte do
slmodem é compilada com `-m32`, pois liga os objetos do DSP x86-32, enquanto
`pri_call` é compilado em 64 bits e depende de DAHDI/libpri. O
`tools/Makefile` gera uma cópia específica de `dsplibs.o` para exportar
`SnrToRetrainTable`, preservando o objeto original. Essa organização separa o
plano de controle do plano de mídia: SIP/PRI estabelece a chamada, enquanto as
pontes transferem as amostras que permitem o treinamento do modem. Essa
separação permite comparar transportes mantendo o mesmo cliente slmodem.

== Topologias experimentais

As topologias 1--3 correspondem ao plano original do projeto; a topologia 4 é
uma extensão posterior, importante para a inferência causal, pois remove o ATA
sem remover o transporte SIP/RTP nem o gateway 2911 (Tabela 3).

#figure(
  table(
    columns: (0.9cm, 4.3cm, 6.3cm, 3.7cm),
    inset: 5pt,
    align: left,
    table.header[ID][Caminho][Função experimental][Estado registrado],
    [1], [Hardmodem \(rarr\) FXS \(rarr\) 2911 \(rarr\) E1 \(rarr\) AS5300], [Validar RAS, PRI, gateway e capacidade V.90 sem variáveis do software cliente.], [6/6 protocolos; sem sondas PTY.],
    [2], [slmodem \(rarr\) SIP/RTP \(rarr\) HT503 \(rarr\) FXS \(rarr\) 2911 \(rarr\) E1 \(rarr\) AS5300], [Validar o slmodem através do ATA e medir o efeito da conversão analógica.], [V.21--V.34 passam; V.90 falha na convergência.],
    [3], [slmodem \(rarr\) Sangoma E1 \(rarr\) AS5300], [Eliminar 2911, ATA e o domínio analógico do caminho de mídia.], [V.21--V.90 passam; V.90 em 56.000 bit/s descendente.],
    [4], [slmodem \(rarr\) SIP/RTP \(rarr\) 2911 \(rarr\) E1 \(rarr\) AS5300], [Isolar o HT503 mantendo SIP e o gateway E1.], [V.21--V.90 passam; quatro chamadas V.90 limpas.],
  ),
  caption: [Topologias experimentais e estado registrado.]
)

== Procedimento de validação de chamadas

Uma chamada só é considerada aprovada quando há `CONNECT` e tráfego
bidirecional completo. O `dialbench` abre a PTY do slmodem e envia `show
clock`; cada resposta deve conter o eco do comando, uma linha com `UTC` e o
prompt `Router>`, e o número padrão de respostas exigidas é três [4, 9]. Um
carrier isolado, portanto, não é confundido com uma conexão de dados
funcional.

Os testes são executados do protocolo mais simples ao mais complexo. A
instrumentação registra a taxa TX/RX, o transporte, o codec, as respostas do
RAS e observações de treinamento. Para o caminho E1, a configuração usa E1
CCS/HDB3/CRC4, com timeslot 16 como canal D e canais B sem cancelamento de
eco [3]; o clock é recuperado do AS5300 pela interface Sangoma. Para o
hardmodem (topologia 1), o protocolo é forçado via `AT+MS=`, disca-se o ramal
e registra-se a taxa autoritativa por `AT&V1` [4].

== Medição de latência do caminho de mídia

Além da validação de protocolo, o `dialbench` mede o atraso e a dispersão do
caminho de mídia com rajadas senoidais, comparando abordagens RTP/SIP sem
envolver o treinamento do modem. Enquanto o teste de dados confirma o enlace e
a resposta do RAS de ponta a ponta, a medição de latência isola o custo e a
variação do caminho de mídia sob um mesmo destino SIP.

O estímulo é um WAV mono s16 a 8 kHz com rajadas de senoide (frequência de
1000 Hz por padrão, rajada de 100 ms a cada 500 ms, amplitude de meia escala e
primeira rajada em `t = 0`), gerado pelo subcomando `gen`. O subcomando
`latency` executa o ciclo completo sobre um caller: garante a existência do
WAV de TX, estabelece a chamada, toca o estímulo no transporte e captura o
retorno de mídia em `audios/<caller>_rx.wav` [9]. O fluxo pode ser executado
como:

```sh
python3 -m dialbench latency <caller> [--freq 1000] [--burst 100] \
    [--period 500] [--dur 10] [--ptime 10]
```

Três callers medem latência sobre SIP/RTP, todos discando o mesmo destino por
padrão (`sip:11@10.42.0.102:5062;transport=udp`), o que mantém a extremidade
distante fixa e isola a implementação de transporte do lado cliente
(Tabela 4) [9].

#figure(
  table(
    columns: (2.4cm, 5.0cm, 7.8cm),
    inset: 5pt,
    align: left,
    table.header[Abordagem][Componente][Papel],
    [baresip], [`tools/baresip_play`], [UA baresip; toca o WAV de TX na chamada SIP e grava o fluxo recebido.],
    [rtp_bridge], [`tools/rtp_bridge`], [Ponte C própria; envia o TX como PCMA em RTP e registra o RX; `--ptime` define o tamanho de quadro.],
    [pjsua], [pjsua (externo)], [UA com áudio nulo; jitter buffer mínimo (`--jb-max-size=0`), latências de captura/reprodução reduzidas e VAD/ec desativados.],
  ),
  caption: [Abordagens RTP/SIP empregadas na medição de latência.]
)

A análise valida o RX como mono s16 a 8 kHz e detecta a cadência de rajadas
por correlação cruzada aplicada ao envelope Goertzel do tom em janelas de
10 ms. O filtro casado mede a energia média de rajada acima da média local, o
que cancela fundo constante (por exemplo, resíduo de harmônicas na frequência
alvo) e marca o início de cada rajada. O atraso de pipeline é estimado por
correlação cruzada dos envelopes de TX e RX, com busca limitada a
`max(500, 2 * período)` ms; um resultado positivo indica RX atrasado em
relação ao TX. Cada rajada TX é pareada com a rajada RX mais próxima da
posição esperada `tx + atraso` dentro de uma margem de `período / 3`; rajadas
sem correspondência são contadas como perdidas. Para cada rajada pareada são
reportados `tx_ms`, `rx_ms` e `delay_ms`, com média, mínimo, máximo e desvio
padrão. O código de saída é 0 quando todas as rajadas são pareadas e 2 quando
há perdas.

// ============================================================
// 4. RESULTADOS
// ============================================================
= Resultados

== Matriz consolidada de protocolos

A Tabela 5 consolida os resultados de conectividade por topologia [4--7]. Na
topologia 1, o marcador ``not-recorded`` para as sondas não indica falha: os
logs do hardmodem e do AS5300 sustentam a validação do enlace, mas não
permitem a mesma comparação com o critério PTY aplicado nas topologias 2--4.

#figure(
  table(
    columns: (0.9cm, 3.1cm, 2.0cm, 2.5cm, 2.4cm, 4.3cm),
    inset: 4pt,
    align: left,
    table.header[Top.][Protocolo][Resultado][TX/RX bit/s][Sondas RAS][Observação],
    [1], [V.21--V.90], [Passa], [300--31.200 / 300--46.667], [Não registrado], [Hardmodem; zero retrains nos logs consolidados.],
    [2], [V.21--V.34], [Passa], [300--24.000 / 300--26.400], [3/3 a 36/36], [HT503 com PCMA; tráfego RAS verificado.],
    [2], [V.90], [Falha], [--], [0/3], [Falha de convergência na fase 4 através do ADC/DAC do ATA.],
    [3], [V.21--V.90], [Passa], [300--33.600 / 300--56.000], [3/3 a 12/12; V.90 5/5], [E1 direto; V.90 validado em 56.000 bit/s descendente.],
    [4], [V.21--V.90], [Passa], [300--31.200 / 300--56.000], [3/3 por chamada], [SIP/RTP direto; quatro chamadas V.90 limpas.],
  ),
  caption: [Matriz consolidada de resultados por topologia e protocolo.]
)

== Diagnóstico espectral da falha V.90 no caminho com ATA

O diretório #link("../analise_v90_sip/")[8] agrega uma diagnose quantitativa
baseada em três capturas de bancada, em um modelo do receptor reconstruído e
em simulações fatoriais. A verificação espectral de V.90 mede as componentes
em 3.600, 4.000 e 4.200 Hz. No caminho E1, as quedas relativas são de
20,04 e 48,72 dB; no SIP digital, de 20,03 e 48,73 dB; ambos os conjuntos
ficam abaixo do critério de codec severo. No caminho com conversão D/A--A/D do
HT503, as quedas sobem para 40,19 e 58,43 dB, ultrapassando o limiar de 28 dB
e acionando o fallback observado na topologia 2.

A identificação do canal estima a perda adicional em relação a 3.600 Hz como
10,1 dB em 3.800 Hz e 21,0 dB em aproximadamente 4.000 Hz. A captura também
mostra uma deriva de atraso de `-0.913` amostra/s, equivalente a
`-114,1 ppm`, ausente no SIP digital direto. O teste de controle com o
fallback severo desabilitado é decisivo para separar as etapas: o receptor
atravessa o verificador inicial, mas falha em seguida no critério de erro do
equalizador e retorna `NO CARRIER`.

A simulação da métrica `avePdsnr` do núcleo reconstruído produz 0,0 para o
caso digital síncrono, 178,8 para o filtro medido com relógios síncronos,
507,4 para o filtro combinado com o relógio residual e 417,7 para a captura
real sem correção. O limiar de fallback é 180; a execução binária controlada
mediu 359,076. Esse resultado aponta para a combinação de filtragem analógica
e relógios assíncronos no HT503, sendo a filtragem suficiente para explicar a
decisão espectral inicial. O jitter de pacotes RTP, por si só, não explica o
resultado: a captura SIP digital teve pacotes contíguos e erro do receptor
convergindo para aproximadamente 3 unidades.

== Resultados de latência

Os valores numéricos de latência por abordagem ainda não estão consolidados
neste relatório; o procedimento descrito na Seção 3.5 define o estímulo, a
instrumentação e as métricas, que devem ser registrados por abordagem e
destino à medida que os experimentos forem executados, como dado complementar
à matriz de protocolos.

// ============================================================
// 5. DISCUSSÃO
// ============================================================
= Discussão

O contraste entre as topologias 2, 3 e 4 é consistente com a hipótese de que o
trecho analógico do HT503 limita o V.90. O mesmo slmodem alcança V.90 no E1
direto (topologia 3) e no SIP/RTP direto para o 2911 (topologia 4), enquanto
o caminho com ATA (topologia 2) alcança V.34, mas não completa a convergência
V.90. Essa conclusão é uma correlação controlada, não uma prova de que apenas
o ADC/DAC seja responsável: jitter, temporização, níveis e configuração do
ATA ainda precisam ser medidos em experimentos adicionais.

A evidência espectral é forte no limite do canal completo entre o gateway e o
cliente, mas não localiza a perda individualmente no conversor D/A ou A/D do
HT503. Também não demonstra que uma taxa de amostragem maior possa recuperar
informação eliminada: converter um fluxo RTP já limitado a 8 kHz para 9,6 kHz
não restaura a banda removida pelo canal. Os resultados separam, contudo, duas
hipóteses concorrentes: a filtragem analógica do ATA, e não o jitter de
pacotes RTP, explica a decisão espectral inicial que aciona o fallback; o
relógio residual assíncrono, combinado ao filtro, explica a queda posterior
no critério do equalizador.

Em relação aos dados end-to-end, ressalva-se que o retrain de fase 4
recuperado antes do `CONNECT` no controle V.90 da topologia 4 não é contado
como falha terminal, pois as três sondas foram concluídas. A assimetria entre
TX e RX observada em V.34/V.90 é esperada pela natureza dos protocolos e não
deve ser interpretada isoladamente como erro.

// ============================================================
// 6. CONCLUSÃO
// ============================================================
= Conclusão

A bancada deixou de ser apenas uma proposta de infraestrutura: o repositório
agrega pontes de áudio, sinalização PRI, orquestração de chamadas e resultados
end-to-end. A evidência atual valida a capacidade do conjunto para V.90 no E1
direto e no SIP/RTP direto e localiza uma limitação específica no caminho que
inclui o HT503, associada à conversão analógica e aos relógios assíncronos do
ATA.

Como trabalhos futuros, recomenda-se consolidar os comandos de reprodução em
um procedimento único (Apêndice A), registrar as métricas de latência por
abordagem, reter logs de todas as chamadas em convenção uniforme e repetir a
topologia 2 com medidas de jitter, nível e temporização do ATA. Em paralelo,
os testes unitários do DSP reconstruído devem continuar sendo relacionados às
chamadas de integração por protocolo e configuração de transporte.

// ============================================================
// REFERÊNCIAS
// ============================================================
#set heading(numbering: none)
= Bibliografia

// Entradas [1]--[9]: artefatos e documentos do próprio repositório.
// Entradas [10]--[12]: referências acadêmicas previstas -- COMPLETAR antes
// da submissão (não foram fabricadas; descrevem apenas o tópico a citar).

[1] Projeto de pesquisa da bancada de testes: servidor de acesso Cisco AS5300,
gateway Cisco 2911, hardmodem de referência e ATA VoIP. Repositório do
projeto, `docs/projeto.pdf`.

[2] Resumo da bancada (projeto, equipamentos, topologias e protocolos).
Repositório do projeto, `docs/project-summary.md`.

[3] Configuração DAHDI/Wanpipe da interface E1 Sangoma. Repositório do
projeto, `docs/dahdi.md`.

[4] Topologia 1 (validação com hardmodem): procedimento, logs e matriz de
resultados. Repositório do projeto, `tests/topology1/`.

[5] Topologia 2 (integração com ATA HT503): procedimento, logs e matriz de
resultados. Repositório do projeto, `tests/topology2/`.

[6] Topologia 3 (E1 direto via Sangoma): procedimento, logs e matriz de
resultados. Repositório do projeto, `tests/topology3/`.

[7] Topologia 4 (SIP/RTP direto para o gateway): procedimento, logs e matriz
de resultados. Repositório do projeto, `tests/topology4/`.

[8] Análise do V.90 sobre E1 direto, SIP direto e SIP com D/A--A/D: capturas,
modelo do receptor e simulações. Repositório do projeto,
`analise_v90_sip/` (inclui `report.pdf` e `README.md`).

[9] Orquestrador `dialbench`: código e manual de uso. Repositório do projeto,
`dialbench/` (inclui `README.md`).

[10] [REFERÊNCIA A COMPLETAR] Recomendação ITU-T V.90 (protocolo de modulação
para transmissão a 56.000 bit/s sobre a rede telefônica pública; citar a
edição e a data de publicação). O V.90 requer terminação digital E1/T1 e
modems digitais no servidor.

[11] [REFERÊNCIA A COMPLETAR] Recomendação ITU-T V.34 (transmissão a
28.800/33.600 bit/s; citar a edição e a data de publicação).

[12] [REFERÊNCIA A COMPLETAR] Documentação e origem do slmodem (Smart Link
Ltd.; núcleo `dsplibs.o` como objeto x86-32; licenciamento BSD e distribuição
em `non-free`). Incluir, se disponível, referência sobre a ausência de
implementações livres completas de V.34/V.90.

// ============================================================
// APÊNDICES
// ============================================================
#pagebreak()

= Apêndice A: Reprodutibilidade do build

Os documentos em `docs/` registram a motivação do projeto, a configuração
DAHDI/Wanpipe, o teste PRI e o resumo das topologias; os arquivos em
`tests/topologyN/` acrescentam procedimentos, logs e matrizes CSV. Os testes
unitários das bibliotecas ficam em `re/test/` e `baresip/test/` e validam as
respectivas bibliotecas, mas não substituem os testes físicos de modem.

Para reproduzir o build das pontes, a sequência mínima documentada é:

```sh
make -C tools
cmake --build re/build -t retest -j
re/build/test/retest -rv
python3 -m dialbench --help
```

As validações de bancada exigem hardware, firmware MICA, DAHDI/Wanpipe,
libpri, permissão de acesso ao dispositivo E1 e a configuração Cisco descrita
em `tests/topology1/` e `tests/topology4/`. Portanto, o resultado físico não é
reproduzível apenas com um checkout e um compilador.
