/*
 * SMS for fun - overlay app pour F4HWN Fusion v7.0.0 (UV-K1 / UV-K5 V3)
 *
 * Messages texte courts entre radios, en FSK (modem BK4829, mêmes réglages
 * que l'app Beam), sur une fréquence saisie dans l'app et mémorisée.
 *
 * Écran CHAT :
 *   historique (police 3x5, 4 lignes par page, le plus récent en bas ; un
 *   message long s'étale sur plusieurs lignes), saisie de 72 caractères
 *   entièrement visible (police 3x5, 3 lignes), légende des touches (2 lignes)
 *     envoyés à gauche, reçus à droite ("INDICATIF:TEXTE")
 *   0-9       saisie multi-tap (2=ABC ... 9=WXYZ, 1=.,'?!-, 0=espace)
 *   * court   mode de saisie ABC -> abc -> 123 (affiché dans la barre du haut)
 *   * long    change la fréquence (appui de 0,8 s)
 *   F         efface le dernier caractère
 *   PTT court envoie le message (MENU aussi)
 *   PTT long  rappelle le dernier message envoyé dans la ligne de saisie
 *   < / >     page précédente (plus ancienne, vers 1) / suivante (plus récente)
 *             La page 1 contient les plus anciens messages ; la dernière, affichée
 *             par défaut, les plus récents.
 *   EXIT      quitte
 * Écran FREQ (18-630 MHz et 840-1300 MHz, comme le firmware) :
 *   0-9       chiffres      *  point décimal   (1 4 5 * 5 MENU = 145.5 MHz)
 *   F         efface        MENU valide        EXIT retour
 *   Hors de la bande du VFO, l'app commute elle-même le filtre d'entrée
 *   VHF/UHF (REG_33) et le gain de l'ampli (REG_36) ; la polarisation de
 *   l'ampli reste celle calibrée pour le VFO.
 *
 * L'historique est au fond de la pile du firmware (à partir de sys_ram_end,
 * fin du .bss) : free_now - STACK_KEEP octets, plafonnés à STACK_MAX. La pile
 * de l'app reste au-dessus avec STACK_KEEP octets de marge ; si elle descendait
 * plus bas, elle n'abîmerait que l'historique, jamais les variables du
 * firmware. À la sortie, la zone reçoit à nouveau le motif 0xA5A5A5A5 que le
 * firmware utilise pour mesurer la pile. L'overlay de 4 Kio est rempli par le
 * code. Taille de l'historique affichée sur l'écran FREQ (MEM).
 * Enregistrements de taille variable, du plus ancien au plus récent :
 *   [longueur][état][indicatif\0][texte\0]
 * Quand la place manque, les plus anciens sont supprimés.
 *
 * Accusé de réception : chaque message porte un numéro ; le poste qui le
 * reçoit répond 'A' à l'expéditeur. Sans réponse en ACK_MS, l'expéditeur
 * renvoie (TRIES essais au total), puis marque le message NON RECU.
 * Un message renvoyé n'est pas affiché deux fois, mais il est re-acquitté.
 *
 * Paquet : 36 mots de 16 bits (72 octets)
 *   w[0]=0xABCD, w[1..33]=charge utile, w[34]=0, w[35]=0xDCBA
 *   charge : type ('M' message, 'A' accusé), 0x04 (version), numéro,
 *            partie (n° | total << 4), indicatif[8], puis 'M' : 36 caractères
 *            du texte / 'A' : indicatif du destinataire[8], zéros
 *   Un message de plus de 36 caractères part en 2 paquets ; l'accusé n'est
 *   envoyé que quand les 2 sont arrivés.
 *   intégrité : CRC matériel du BK4829 (REG_0B bit 4)
 */
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"

#define DLY       0xFF          /* pseudo-registre : pause en ms */
#define WORDS     36
#define P_START   0xABCD
#define P_END     0xDCBA
#define CALL_LEN  8
#define TXT_LEN   72            /* 4 lignes de 18 caractères */
#define PART      36            /* caractères par paquet */
#define COLS      32            /* caractères par ligne en police 3x5 */
#define ROWS      4             /* lignes d'historique par page (pas de 6 px) */
#define SPLIT     28000000u     /* 280 MHz (pas de 10 Hz) : limite VHF/UHF */
#define F_MIN     1800000u      /* plages du BK4829 (frequencies.c) */
#define F_GAP_LO  63000000u
#define F_GAP_HI  84000000u
#define F_MAX     130000000u
#define TAP_MS    900
#define HOLD_MS   800           /* appui long sur * ou PTT */
#define PROTO     0x04          /* version du protocole */
#define B_ID      2             /* position dans la charge utile */
#define B_PART    3
#define B_CALL    4
#define B_DATA    12
#define ACK_MS    2000          /* attente d'un accusé après chaque envoi */
#define ACK_DELAY 150           /* le récepteur laisse l'émetteur repasser en RX */
#define TRIES     3             /* envois au total */
#define STACK_KEEP 1536         /* pile laissée à l'app et au firmware */
#define STACK_MAX 4000

enum { ST_NONE, ST_TX, ST_WAIT, ST_BUSY, ST_BAND, ST_ERR, ST_ACK, ST_FAIL };
enum { H_SENT, H_RX, H_OK, H_FAIL };     /* état d'une entrée d'historique */
enum { M_CHAT, M_FREQ };

/* Textes en flash (assets, voir gen_assets.py) : ils ne prennent pas de RAM
 * d'overlay, qui reste à l'historique. Chaînes de taille fixe, terminées par 0. */
#define AS_HELP   0             /* 6 x 33 : légende CHAT (2), FREQ (3), titre FREQ */
#define AS_ST     198           /* 8 x 8  : textes d'état */
#define AS_CASE   262           /* 3 x 4  : ABC / abc / 123 */
#define AS_T9     274           /* 10 x 8 : lettres des touches 0-9 */
#define AS_MARK   354           /* 4 x 10 : marque des messages envoyés par état */
/* Séquences BK4829 (registre, valeur) reprises de l'app Beam, en assets aussi :
 * position en octets et nombre de paires. Registre 0xFF = pause en ms. */
#define SEQ_SETUP   394, 10     /* SetupAircopy + ResetFSK (4 dernières paires) */
#define SEQ_RESET   418, 4
#define SEQ_RX      434, 8
#define SEQ_TX_LOAD 466, 4
#define SEQ_TX_GO   482, 2
#define SEQ_TX_END  490, 7


typedef struct {                /* petits champs d'abord : accès Thumb plus courts */
  uint8_t txtLen, fLen, tapKey, tapIdx, rxIdx, mode, status, page, caseMode;
  uint8_t seq, tries, lastId;   /* numéro envoyé, essais, dernier numéro reçu */
  bool running, dirty, holdFired, waiting;
  uint16_t cap, used, count;    /* historique : octets dispo, utilisés, messages */
  uint16_t *rx;                 /* tampon de réception (sur la pile de app_main) */
  uint8_t *hist;                /* zone d'historique */
  char *txt;                    /* saisie (pile) */
  char *asmb;                   /* réassemblage de la 1re partie reçue (pile) */
  uint8_t asmId, asmOk;
  char asmCall[CALL_LEN];
  char lastCall[CALL_LEN];      /* expéditeur du dernier message reçu */
  uint32_t freq, tapTime, holdTime, ackTime;
  char fdig[11];                /* saisie : jusqu'à 4 chiffres . 5 décimales */
  char me[CALL_LEN + 1];
} state_t;

static state_t S;               /* .bss NOLOAD (app.ld) : hors du fichier .app */
static const app_api_t *api_g;
#define API const app_api_t *const A = api_g

#define HIST S.hist

static void asset(void *buf, unsigned off, unsigned len) {
  API;
  A->asset_read((uint16_t)off, buf, (uint16_t)len);
}

#define RUN(s) run(s)
static void run(unsigned off, unsigned n) {
  API;
  for (; n; n--, off += 4) {
    uint16_t r[2];
    asset(r, off, 4);
    if (r[0] == DLY) A->delay_ms(r[1]);
    else A->bk_write((uint8_t)r[0], r[1]);
  }
}

static void copy(char *d, const char *s, unsigned n) {
  while (n--) *d++ = *s++;
}

static bool same(const char *a, const char *b, unsigned n) {
  while (n--) if (*a++ != *b++) return false;
  return true;
}

/* Copie au plus n caractères jusqu'au 0, renvoie la fin */
static char *cat(char *d, const char *s, unsigned n) {
  while (n-- && *s) *d++ = *s++;
  return d;
}

/* Registres 0x38/0x39 (pas de 10 Hz) + filtre d'entrée VHF (0x04) / UHF (0x08)
 * dans REG_33, comme BK4819_PickRXFilterPathBasedOnFrequency */
static void set_freq(uint32_t f) {
  API;
  A->bk_write(0x38, (uint16_t)f);
  A->bk_write(0x39, (uint16_t)(f >> 16));
  A->bk_write(0x33, (uint16_t)((A->bk_read(0x33) & ~0x0Cu) | (f < SPLIT ? 0x04u : 0x08u)));
}

static void tune(uint32_t f) {               /* change de fréquence en restant en TX/RX */
  API;
  set_freq(f);
  uint16_t r = A->bk_read(0x30);
  A->bk_write(0x30, 0);
  A->bk_write(0x30, r);
}

static void rx_start(void) {
  RUN(SEQ_SETUP);
  set_freq(S.freq);
  RUN(SEQ_RX);                               /* REG_30 0 -> 0xBFF1 : recale le PLL */
  S.rxIdx = 0;
}

/* k-ième enregistrement en partant du plus récent (k < count) */
static uint8_t *rec_at(unsigned k) {
  uint8_t *r = HIST;
  for (unsigned i = S.count - 1u - k; i; i--) r += r[0];
  return r;
}

static char *rec_call(uint8_t *r) { return (char *)r + 2; }

static char *rec_txt(uint8_t *r) {
  char *c = rec_call(r);
  while (*c++) ;
  return c;
}

/* Le message en attente d'accusé : le seul enregistrement à l'état H_SENT */
static uint8_t *pending(void) {
  for (unsigned k = 0; k < S.count; k++) {
    uint8_t *r = rec_at(k);
    if (r[1] == H_SENT) return r;
  }
  return 0;
}

/* Ajoute un enregistrement ; supprime les plus anciens s'il manque de la place */
static void hist_add(uint8_t st, const char *call, const char *txt) {
  unsigned lc = 0;
  while (lc < CALL_LEN && call[lc]) lc++;
  unsigned lt = 0;
  while (lt < TXT_LEN && txt[lt]) lt++;
  unsigned len = 4 + lc + lt;
  if (len > S.cap) return;
  while (S.count && S.used + len > S.cap) {
    unsigned d = HIST[0];
    S.used -= d;
    S.count--;
    copy((char *)HIST, (char *)HIST + d, S.used);
  }
  uint8_t *r = HIST + S.used;
  r[0] = (uint8_t)len;
  r[1] = st;
  char *e = cat(rec_call(r), call, lc);
  *e++ = 0;
  *cat(e, txt, lt) = 0;
  S.used += len;
  S.count++;
  S.page = 0;
}

/* Émet un paquet : type 'M' (data = texte, 36 car. max) ou 'A' (data = indicatif
 * du destinataire, 8 car.). Renvoie false si l'émission est bloquée ou échoue. */
static bool xmit(char type, const char *data, unsigned len, uint8_t id, uint8_t part) {
  API;
  uint16_t p[WORDS];
  char *b = (char *)&p[1];

  if (A->tx_state()) { S.status = ST_BUSY; return false; }

  for (int i = 0; i < WORDS; i++) p[i] = 0;
  p[0] = P_START;
  p[35] = P_END;
  b[0] = type;
  b[1] = PROTO;
  b[B_ID] = (char)id;
  b[B_PART] = (char)part;
  copy(b + B_CALL, S.me, CALL_LEN);
  cat(b + B_DATA, data, len);

  RUN(SEQ_SETUP);
  A->tx_set_params();                        /* puissance/calibration du VFO, PA actif */
  tune(S.freq);
  /* gain de l'ampli selon la bande, comme BK4819_SetupPowerAmplifier */
  A->bk_write(0x36, (uint16_t)((A->bk_read(0x36) & 0xFFC0u) | (S.freq < SPLIT ? 0x08u : 0x22u)));
  RUN(SEQ_TX_LOAD);
  for (int i = 0; i < WORDS; i++) A->bk_write(0x5F, p[i]);
  RUN(SEQ_TX_GO);
  int t = WORDS * 3 + 100;
  while (t-- > 0 && !(A->bk_read(0x0C) & 1)) A->delay_ms(5);
  RUN(SEQ_TX_END);
  A->tx_end();
  rx_start();
  return t > 0;
}

/* Un essai d'envoi du message en attente, puis attente de l'accusé */
static void attempt(void) {
  API;
  uint8_t *r = pending();
  if (!r) { S.waiting = false; return; }
  S.tries++;
  char *t = rec_txt(r);
  unsigned n = 0;
  while (t[n]) n++;
  unsigned total = n > PART ? 2u : 1u;
  for (unsigned i = 0; i < total; i++) {
    if (!xmit('M', t + PART * i, PART, S.seq, (uint8_t)(i | total << 4)) && S.status == ST_BUSY) {
      S.waiting = false;                     /* TX interdit : inutile d'insister */
      r[1] = H_FAIL;
      return;
    }
  }
  S.status = ST_WAIT;
  S.ackTime = A->ticks_ms();
}

/* Nouveau message : entre dans l'historique (en attente), premier essai */
static void send(void) {
  API;
  if (A->tx_state()) { S.status = ST_BUSY; return; }
  S.txt[S.txtLen] = 0;
  hist_add(H_SENT, "", S.txt);
  S.seq++;
  S.tries = 0;
  S.waiting = true;
  S.txtLen = 0;
  S.txt[0] = 0;
  attempt();
}

static void got_packet(void) {
  API;
  uint16_t *p = S.rx;
  char *b = (char *)&p[1];

  uint8_t id = (uint8_t)b[B_ID];

  if (p[0] != P_START || p[35] != P_END || b[1] != PROTO) return;
  if (b[0] == 'A') {                         /* accusé : pour moi, pour ce message ? */
    uint8_t *r = pending();
    if (S.waiting && r && id == S.seq && same(b + B_DATA, S.me, CALL_LEN)) {
      S.waiting = false;
      r[1] = H_OK;
      S.status = ST_ACK;
      S.dirty = true;
      A->play_tone(1200, 60);
    }
    return;
  }
  if (b[0] != 'M') return;
  char *txt = b + B_DATA;
  b[B_DATA + PART] = 0;
  unsigned part = (uint8_t)b[B_PART];
  if (part == 0x20) {                        /* 1re de 2 parties : on la garde */
    copy(S.asmb, txt, PART);
    copy(S.asmCall, b + B_CALL, CALL_LEN);
    S.asmId = id;
    S.asmOk = 1;
    return;
  }
  if (part == 0x21) {                        /* 2e partie : recolle à la 1re */
    if (!S.asmOk || S.asmId != id || !same(S.asmCall, b + B_CALL, CALL_LEN)) return;
    copy(S.asmb + PART, txt, PART + 1);
    txt = S.asmb;
    S.asmOk = 0;
  }
  if (id != S.lastId || !same(b + B_CALL, S.lastCall, CALL_LEN)) {   /* pas un renvoi */
    S.lastId = id;
    copy(S.lastCall, b + B_CALL, CALL_LEN);
    hist_add(H_RX, b + B_CALL, txt);
    S.dirty = true;
    A->backlight_on();
    A->play_tone(880, 80);
  }
  A->delay_ms(ACK_DELAY);
  char to[CALL_LEN];
  copy(to, b + B_CALL, CALL_LEN);           /* xmit relance la réception */
  xmit('A', to, CALL_LEN, id, 0x10);         /* répond à l'expéditeur */
}

static void rx_poll(void) {
  API;
  if (!(A->bk_read(0x0C) & 1)) return;
  A->bk_write(0x02, 0);
  uint16_t st = A->bk_read(0x02);
  if (st & (3u << 12)) {                     /* FIFO presque plein / paquet fini */
    unsigned n = (st & (1u << 13)) ? WORDS : 4;
    while (n-- && S.rxIdx < WORDS) S.rx[S.rxIdx++] = A->bk_read(0x5F);
  }
  if (S.rxIdx >= WORDS) {
    if (!(A->bk_read(0x0B) & 0x10)) got_packet();   /* bit 4 = erreur CRC */
    rx_start();
  }
}

/* P10(i) = 10^(8-i) : 100000000 ... 1 */
static uint32_t P10(unsigned i) {
  uint32_t r = 1;
  while (i++ < 8) r *= 10;
  return r;
}

/* "145.5" -> 14550000 (pas de 10 Hz) */
static uint32_t parse_freq(void) {
  uint32_t mhz = 0, dec = 0;
  int nd = -1;                               /* -1 : avant le point */
  for (int i = 0; i < S.fLen; i++) {
    char c = S.fdig[i];
    if (c == '.') nd = 0;
    else if (nd < 0) mhz = mhz * 10 + (uint32_t)(c - '0');
    else dec += (uint32_t)(c - '0') * P10(4 + nd++);
  }
  return mhz * 100000u + dec;
}

/* 14550000 -> "145.5000" (dec décimales) ; 4 chiffres au-delà de 1000 MHz */
static void fmt_freq(char *s, uint32_t f, int dec) {
  char *b = s;
  for (int i = 0; i < 4 + dec; i++) {
    char c = '0';
    uint32_t p = P10(i);
    while (f >= p) { f -= p; c++; }
    if (c != '0' || i >= 3 || s != b) *s++ = c;   /* pas de zéros de tête */
    if (i == 3) *s++ = '.';
  }
  *s = 0;
}

/* Plages accordables par le BK4829 (trou 630-840 MHz) */
static bool freq_ok(uint32_t f) {
  return f >= F_MIN && f <= F_MAX && (f < F_GAP_LO || f >= F_GAP_HI);
}

/* Texte affiché d'un enregistrement : "INDICATIF:TEXTE" (reçu) ou
 * "TEXTE ..."/" OK"/" NON RECU" (envoyé). Renvoie la longueur. */
static unsigned rec_line(uint8_t *r, char *s) {
  char t[10];
  char *e = s;
  if (r[1] == H_RX && *rec_call(r)) { e = cat(e, rec_call(r), CALL_LEN); *e++ = ':'; }
  e = cat(e, rec_txt(r), TXT_LEN);
  if (r[1] != H_RX) {
    asset(t, AS_MARK + 10u * r[1], 10);
    e = cat(e, t, 10);
  }
  *e = 0;
  return (unsigned)(e - s);
}

/* Découpe s (n car.) en lignes de COLS max, coupées après un espace parmi les 8
 * derniers caractères si possible. st[j] = début de la ligne j, st[lignes] = n.
 * Chaque ligne fait au moins 24 caractères : 4 lignes au plus pour 90. */
static unsigned wrap(const char *s, unsigned n, uint8_t *st) {
  unsigned rows = 0, p = 0;
  for (;;) {
    st[rows++] = (uint8_t)p;
    if (n - p <= COLS) break;
    unsigned cut = p + COLS;                 /* ligne = p..cut-1, 32 car. max */
    for (unsigned i = cut - 1; i > p + COLS - 9; i--)
      if (s[i] == ' ') { cut = i + 1; break; }   /* l'espace finit la ligne */
    p = cut + (s[cut] == ' ');
  }
  st[rows] = (uint8_t)n;
  return rows;
}

static unsigned pages(void) {                /* sans division (pas de libgcc) */
  char s[CALL_LEN + TXT_LEN + 12];
  uint8_t st[6];
  int rows = 0;
  for (unsigned k = 0; k < S.count; k++) {
    uint8_t *r = rec_at(k);
    rows += (int)wrap(s, rec_line(r, s), st);
  }
  unsigned n = 0;
  for (; rows > 0; rows -= ROWS) {
    n++;
    __asm__("" : "+r"(rows));                /* garde la boucle (sinon __aeabi_uidiv) */
  }
  return n;
}

/* PTT long : recopie le dernier message envoyé (le plus récent de l'historique) */
static void recall(void) {
  API;
  for (unsigned k = 0; k < S.count; k++) {
    uint8_t *r = rec_at(k);
    if (r[1] == H_RX) continue;
    S.txtLen = (uint8_t)(cat(S.txt, rec_txt(r), TXT_LEN) - S.txt);
    S.txt[S.txtLen] = 0;
    S.tapKey = APP_KEY_INVALID;
    A->play_tone(1200, 40);
    return;
  }
}

static void key_freq(uint8_t k) {
  API;
  int dot = -1;
  for (int i = 0; i < S.fLen; i++) if (S.fdig[i] == '.') dot = i;
  if (k <= APP_KEY_9) {
    if (dot < 0 ? S.fLen < 4 : S.fLen - dot <= 5) S.fdig[S.fLen++] = '0' + k;
  } else if (k == APP_KEY_STAR) {
    if (dot < 0 && S.fLen) S.fdig[S.fLen++] = '.';
  } else if (k == APP_KEY_F) {
    if (S.fLen) S.fLen--;
  } else if (k == APP_KEY_MENU) {
    uint32_t f = parse_freq();
    if (!freq_ok(f)) { S.status = ST_BAND; A->play_tone(500, 80); return; }
    S.freq = f;
    S.mode = M_CHAT;
    rx_start();
  } else if (k == APP_KEY_EXIT) {
    if (S.freq) S.mode = M_CHAT; else S.running = false;
  }
}

static void key_chat(uint8_t k) {
  API;
  if (k <= APP_KEY_9) {
    char t[8];
    asset(t, AS_T9 + 8u * k, 8);
    uint32_t now = A->ticks_ms();
    if (S.caseMode == 2) {                   /* 123 : chiffre direct */
      S.tapKey = APP_KEY_INVALID;
      if (S.txtLen < TXT_LEN) { S.txt[S.txtLen++] = '0' + k; S.txt[S.txtLen] = 0; }
      return;
    }
    if (k == S.tapKey && now - S.tapTime < TAP_MS && S.txtLen) {   /* multi-tap */
      if (!t[++S.tapIdx]) S.tapIdx = 0;
      S.txtLen--;
    } else if (S.txtLen < TXT_LEN) {
      S.tapKey = k;
      S.tapIdx = 0;
    } else {
      S.tapKey = APP_KEY_INVALID;            /* plein : pas de multi-tap sur le dernier */
      return;
    }
    char c = t[S.tapIdx];
    if (S.caseMode == 1 && c >= 'A') c += 'a' - 'A';             /* abc */
    S.txt[S.txtLen++] = c;
    S.txt[S.txtLen] = 0;
    S.tapTime = now;
    return;
  }
  S.tapKey = APP_KEY_INVALID;
  if (k == APP_KEY_F) { if (S.txtLen) S.txt[--S.txtLen] = 0; }
  else if (k == APP_KEY_MENU) { if (S.txtLen && !S.waiting) S.status = ST_TX; }
  else if (k == APP_KEY_UP) { if (S.page + 1u < pages()) S.page++; }     /* < : vers la page 1 (anciens) */
  else if (k == APP_KEY_DOWN) { if (S.page) S.page--; }                    /* > : pages récentes */
  else if (k == APP_KEY_EXIT) S.running = false;
}

static void draw(void) {
  API;
  char s[CALL_LEN + TXT_LEN + 12];
  char t[33];

  A->display_clear();
  A->status_clear();
  if (S.mode == M_FREQ) {
    asset(t, AS_HELP + 33u * 5, 33);
    A->print_normal(t, 0, 127, 0);
    copy(s, S.fdig, S.fLen);
    s[S.fLen] = '_';
    s[S.fLen + 1] = 0;
    A->print_normal(s, 0, 127, 2);
    *A->append_u32(s, S.cap) = 0;            /* taille de l'historique, après "MEM:" */
    A->print_tiny(s, 110, 36, false, true);
    for (int i = 0; i < 3; i++) {
      asset(t, AS_HELP + 33u * (2 + i), 33);
      A->print_tiny(t, 0, (uint8_t)(36 + 7 * i), false, true);
    }
  } else {
    /* barre d'état : fréquence, page */
    fmt_freq(s, S.freq, 3);                  /* 3 décimales dans la barre d'état */
    A->print_tiny(s, 0, 1, true, true);
    if (S.count) {                           /* page / pages, calé à gauche de la batterie */
      unsigned n = pages();                  /* page 1 = la plus ancienne */
      char *e = A->append_u32(s, n - S.page);
      *e++ = '/';
      e = A->append_u32(e, n);
      *e = 0;
      A->print_tiny(s, (uint8_t)(83 - 4 * (e - s)), 1, true, true);
    }
    /* historique : lignes comptées depuis le bas (plus récent), 2 lignes
     * pour un message de plus de 32 caractères */
    unsigned first = S.page * ROWS, row = 0;
    for (unsigned k = 0; k < S.count && row < first + ROWS; k++) {
      uint8_t *r = rec_at(k);
      uint8_t st[6];
      unsigned lines = wrap(s, rec_line(r, s), st);
      unsigned w = 0;                        /* ligne la plus longue : un reçu est */
      for (unsigned j = 0; j < lines; j++)   /* un bloc calé à droite, lignes alignées */
        if (st[j + 1] - st[j] > w) w = st[j + 1] - st[j];
      unsigned x = r[1] == H_RX ? 128u - 4u * w : 0u;
      for (unsigned j = lines; j-- > 0; row++) {   /* dernière ligne d'abord */
        if (row < first || row >= first + ROWS) continue;   /* hors de la page */
        char *seg = s + st[j];
        unsigned sl = st[j + 1] - st[j];
        char c = seg[sl];
        seg[sl] = 0;
        A->print_tiny(seg, (uint8_t)x, (uint8_t)(6 * (ROWS - 1 - (row - first))), false, true);
        seg[sl] = c;
      }
    }
    /* saisie complète en police 3x5 (3 lignes max), curseur _ */
    copy(s, S.txt, S.txtLen);
    unsigned v = S.txtLen;
    if (v < TXT_LEN) s[v++] = '_';
    uint8_t st[6];
    unsigned lines = wrap(s, v, st);
    s[v] = 0;
    for (unsigned j = 0; j < lines; j++) {
      char c = s[st[j + 1]];
      s[st[j + 1]] = 0;                      /* coupe la ligne le temps de l'afficher */
      A->print_tiny(s + st[j], 0, (uint8_t)(24 + 6 * j), false, true);
      s[st[j + 1]] = c;
    }
    for (int i = 0; i < 2; i++) {
      asset(t, AS_HELP + 33u * i, 33);
      A->print_tiny(t, 0, (uint8_t)(41 + 8 * i), false, true);
    }
    asset(t, AS_CASE + 4u * S.caseMode, 4); /* mode de saisie, barre du haut */
    A->print_tiny(t, 32, 1, true, true);
  }
  {
    asset(t, AS_ST + 8u * S.status, 8);
    if (S.status == ST_WAIT) t[4] = (char)('0' + S.tries);   /* "ATT n/3" */
    A->print_tiny(t, 46, 1, true, true);
  }
  A->draw_battery();
  A->blit_full();
  A->blit_status();
}

/* Boucle principale */
__attribute__((noinline)) static void loop(void) {
  API;
  uint8_t prev = APP_KEY_INVALID;
  unsigned tick = 0;
  while (S.running) {
    uint8_t k = A->get_key();
    if (k == APP_KEY_SAVER || k == APP_KEY_WAKE) {
      S.dirty |= (k == APP_KEY_WAKE);
      k = APP_KEY_INVALID;
    } else if (k != prev && k != APP_KEY_INVALID) {
      A->backlight_on();
      if (!S.waiting) S.status = ST_NONE;
      if (S.mode == M_FREQ) key_freq(k); else key_chat(k);
      S.dirty = true;
    }
    if (S.mode == M_CHAT) {                  /* appuis courts / longs sur * et PTT */
      uint32_t now = A->ticks_ms();
      if (k == APP_KEY_STAR || k == APP_KEY_PTT) {
        if (prev != k) { S.holdTime = now; S.holdFired = false; }
        else if (!S.holdFired && now - S.holdTime >= HOLD_MS) {
          S.holdFired = true;                /* long */
          if (k == APP_KEY_PTT) recall();
          else { S.mode = M_FREQ; S.fLen = 0; }
          S.dirty = true;
        }
      } else if ((prev == APP_KEY_STAR || prev == APP_KEY_PTT) && !S.holdFired) {
        if (prev == APP_KEY_PTT) {           /* court, au relâchement */
          if (S.txtLen && !S.waiting) S.status = ST_TX;
        } else {
          if (++S.caseMode > 2) S.caseMode = 0;
          S.tapKey = APP_KEY_INVALID;
        }
        S.dirty = true;
      }
    }
    prev = k;
    if (S.mode == M_CHAT) {
      rx_poll();
      if (S.waiting && A->ticks_ms() - S.ackTime >= ACK_MS) {   /* pas d'accusé */
        if (S.tries < TRIES) S.status = ST_TX;
        else {
          uint8_t *r = pending();
          if (r) r[1] = H_FAIL;
          S.waiting = false;
          S.status = ST_FAIL;
          A->play_tone(400, 150);
        }
        S.dirty = true;
      }
    }
    if (S.dirty) {
      S.dirty = false;
      draw();
      if (S.status == ST_TX) {               /* "TX..." est déjà affiché */
        if (S.waiting) attempt(); else send();
        S.dirty = true;
      }
    }
    A->delay_ms(10);
    A->backlight_update();
    if (++tick >= 100) { tick = 0; A->battery_sample(); S.dirty = true; }
  }

}

/* Tampons de saisie, de réassemblage et de réception sur la pile, puis boucle */
__attribute__((noinline)) static void on_stack(void) {
  uint16_t rxbuf[WORDS];
  char txt[TXT_LEN + 2];
  char asmb[TXT_LEN + 2];
  S.rx = rxbuf;
  S.txt = txt;
  S.asmb = asmb;
  txt[0] = 0;
  loop();
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api) {
  uint8_t *z = (uint8_t *)&S;
  for (unsigned i = 0; i < sizeof S; i++) z[i] = 0;
  api_g = api;
  API;
  S.tapKey = APP_KEY_INVALID;
  S.running = S.dirty = true;

  S.seq = (uint8_t)A->rand32();             /* numéros différents à chaque lancement */
  A->boot_callsign(S.me, CALL_LEN + 1);
  S.me[CALL_LEN] = 0;

  A->cfg_load((uint8_t *)&S.freq, 4);
  if (freq_ok(S.freq)) rx_start();
  else { S.freq = 0; S.mode = M_FREQ; }
  A->backlight_on();

  /* historique au fond de la pile, STACK_KEEP octets sous la pile courante */
  uint32_t st = A->sys_stack_free_now();
  st = st > STACK_KEEP ? st - STACK_KEEP : 0;
  S.cap = (uint16_t)((st > STACK_MAX ? STACK_MAX : st) & ~3u);
  S.hist = (uint8_t *)(((uintptr_t)A->sys_ram_end + 3u) & ~(uintptr_t)3u);
  on_stack();
  for (uint32_t i = 0; i < S.cap; i += 4)    /* remet le motif de mesure de pile */
    *(uint32_t *)(S.hist + i) = 0xA5A5A5A5u;

  if (S.freq) A->cfg_save((const uint8_t *)&S.freq, 4);
  RUN(SEQ_RESET);
  tune(A->rx_freq());                        /* rend le synthé sur la fréquence du VFO */
}
