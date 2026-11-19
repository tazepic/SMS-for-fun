# SMS for fun : app overlay pour F4HWN Fusion v7.0.0

Cette app envoie et reçoit de courts messages texte entre UV-K1 / UV-K5 V3, en FSK, sur une fréquence que tu saisis dans l'app.

- Nom court (11 caractères) pour tenir dans le menu des apps.
- Fichier prêt à installer : `SMSforfun.app` v2.3 (en-tête 64 + code 3988 + 518 octets de textes et de séquences radio en flash).
- Fréquences : toute la plage du firmware, 18–630 MHz et 840–1300 MHz.
- Accusé de réception : jusqu'à 3 envois, puis « NON RECU ». **Protocole v4 : toutes les radios doivent avoir la v1.8 ou plus.**
- Messages : 72 caractères. La saisie est entièrement visible, en petite police sur 3 lignes. Au-delà de 36 caractères, le message part en 2 paquets.
- Historique : chaque message n'occupe que sa longueur réelle. L'historique est au **fond** de la pile du firmware, juste après la fin du `.bss` (`sys_ram_end`). Sa taille est la pile libre au lancement moins 1536 octets, dans la limite de 4000 octets. Elle s'affiche sur l'écran de fréquence (`MEM: 1234`). Si la pile de l'app descendait trop, elle n'abîmerait que l'historique, jamais les variables du firmware. En quittant, l'app remet le motif `0xA5A5A5A5` qui sert à mesurer la pile. Quand la place manque, les plus anciens messages sont supprimés. L'historique est effacé quand on quitte l'app.
- API : niveau 4 (`--api-min 4`), sans asset et sans capacité requise.
- La fréquence est mémorisée entre deux lancements (`cfg_save`).

## Recompiler (facultatif)

```sh
git clone https://github.com/armel/uv-k1-k5v3-firmware-custom
cd uv-k1-k5v3-firmware-custom
git checkout v7.0.0
mkdir App/apps/smsfriends
cp smsfriends_app.c gen_assets.py build.sh app.ld App/apps/smsfriends/
chmod +x App/apps/smsfriends/build.sh
./compile-app.sh smsfriends        # -> build/Apps/SMSforfun.app
```

Installe `SMSforfun.app` depuis UV Studio : **Apps → Install a local app**. Sur la radio, lance-la avec **F + 7 (VOX)**, puis choisis SMS for fun et appuie sur **M**.

## Utilisation

Écran **FREQ** (il s'ouvre au premier lancement) :

| Touche | Action |
|---|---|
| 0-9 | Chiffres de la fréquence en MHz |
| * | Point décimal : `1 4 5 * 5` = 145.5 MHz, `1 2 9 6 * 2` = 1296.2 MHz |
| F | Efface le dernier chiffre |
| MENU | Valide |
| EXIT | Retour (ou quitte si aucune fréquence n'est encore réglée) |

Écran **CHAT** :

| Touche | Action |
|---|---|
| 0-9 | Saisie multi-tap : 2=ABC … 9=WXYZ, 1=`.,'?!-`, 0=espace. Le chiffre vient en dernier dans le cycle. En mode 123, chaque touche tape directement son chiffre |
| * (appui court) | Mode de saisie : ABC → abc → 123, affiché dans la barre du haut |
| * (appui long, 0,8 s) | Change la fréquence |
| F | Efface le dernier caractère |
| PTT (appui court) ou MENU | Envoie le message (72 caractères max, 4 lignes) |
| PTT (appui long, 0,8 s) | Remet le dernier message envoyé dans la ligne de saisie (bip court) : un PTT court le renvoie |
| < / > (haut / bas sur UV-K5 V3) | Page précédente (plus ancienne, vers la page 1) / page suivante (plus récente). La page 1 contient les plus anciens messages. La dernière page, affichée par défaut et à chaque nouveau message, contient les plus récents. |
| EXIT | Quitte |

La barre du haut affiche la fréquence (3 décimales), le mode de saisie (ABC/abc/123), l'état (TX..., ATT 1/3, RECU, ECHEC, BLOQUE, BANDE?) et, à gauche de la batterie, la page courante sur le nombre de pages (`2/4`, la page 1 étant la plus ancienne). En dessous, l'historique s'affiche en petite police sur 4 lignes par page, avec le message le plus récent en bas. Un message long tient sur 2 lignes, coupées entre deux mots : les messages envoyés sont à gauche, suivis de `...` (en attente), `OK` (accusé reçu) ou `NON RECU`. Les messages reçus sont à droite (`INDICATIF:TEXTE`), en bloc : toutes leurs lignes commencent à la même colonne. La saisie vient ensuite, en entier, en petite police, puis une légende de 2 lignes. Les bips signalent un message reçu (880 Hz), un accusé (1200 Hz) et un échec (400 Hz).

## Fréquences

- **Plage acceptée :** celle du BK4829 dans le firmware, 18–630 MHz et 840–1300 MHz. Entre 630 et 840 MHz, la puce ne sait pas accorder et l'écran affiche « BANDE? ».
- **Commutation de bande :** l'app commute elle-même le filtre d'entrée VHF/UHF (REG_33) et le gain de l'ampli (REG_36) selon la fréquence. Le VFO peut donc être sur n'importe quelle bande.
- **Puissance d'émission :** la polarisation de l'ampli reste celle calibrée pour le VFO. Pour une puissance correcte, mets le VFO sur la même bande que l'app. En dehors d'environ 136–174 et 400–480 MHz, l'ampli de l'UV-K1 n'est pas conçu pour émettre : peu de puissance, et des harmoniques.
- **Verrou d'émission :** `tx_state` vérifie le VFO (modulation FM, batterie, verrou TX), pas la fréquence de l'app.
- **Réglages du VFO :** mets-le en FM narrow sur les deux radios.
- **Modem :** celui de l'app Beam (paquet de 72 octets, CRC matériel). Il n'y a ni accusé de réception ni répétition.

## Accusé de réception

1. L'expéditeur envoie un message `M` numéroté. Le numéro est tiré au hasard au lancement, puis augmente de 1 à chaque message.
2. Chaque poste qui reçoit ce message attend 150 ms, puis répond par un accusé `A` qui contient l'indicatif de l'expéditeur et le numéro du message.
3. Sans accusé dans les 2 s, l'expéditeur renvoie le même message (3 envois au total). Après le 3e échec, le message est marqué `NON RECU`.
4. Si le destinataire reçoit un renvoi (même indicatif, même numéro), il ne l'affiche pas une deuxième fois, mais il renvoie l'accusé.
5. Pendant l'attente d'un accusé, un nouvel envoi est refusé.

## Protocole

Chaque paquet fait 36 mots de 16 bits : `0xABCD`, puis la charge utile de 66 octets, puis `0x0000`, puis `0xDCBA`. L'intégrité est vérifiée par le CRC matériel du BK4829.

La charge utile contient, dans l'ordre :
- le type : `'M'` pour un message, `'A'` pour un accusé ;
- la version `0x04` ;
- le numéro du message ;
- la partie : numéro de la partie, plus le nombre total de parties multiplié par 16 (`0x10` pour un message en 1 paquet, `0x20` et `0x21` pour les 2 paquets d'un message en 2 parties) ;
- l'indicatif de l'émetteur (8 octets) ;
- pour un message, 36 caractères du texte ; pour un accusé, l'indicatif du destinataire (8 octets) ;
- des zéros pour compléter.

Le destinataire garde la 1re partie et attend la 2e (même indicatif, même numéro). Il n'envoie l'accusé que quand le message est complet.

## Fichiers

- `smsfriends_app.c` : l'app (point d'entrée `app_main(const app_api_t *)`).
- `app.ld` : le script de linker. Il ressemble à celui de Beam, mais `.bss` est en NOLOAD (code et variables : 4 Ko au total).
- `gen_assets.py` : les textes (légendes, états, lettres des touches) et les séquences de registres FSK, stockés en flash et lus avec `asset_read` pour laisser la RAM à l'historique.
- `build.sh` : compile avec clang `-Oz` et ld.lld s'ils sont présents. Avec GCC, le code peut dépasser les 4 Ko.

