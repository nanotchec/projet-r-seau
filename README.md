# Projet anneau - partie Comm

Cette base implémente une version simple et fonctionnelle de `Comm` en C, avec une socket locale Unix pour parler au `Driver`.

## Fichiers utiles

- `build/bin/comm` : interface utilisateur en ligne de commande
- `build/bin/mock_driver` : faux driver local pour tester l'intégration
- `include/protocol.h` : contrat partagé `Comm <-> Driver`

## Build

```sh
make
```

## Test local rapide

Terminal 1 :

```sh
./build/bin/mock_driver
```

Terminal 2 :

```sh
./build/bin/comm
```

Commandes disponibles :

```text
join nodeA 127.0.0.1 9000
send nodeB bonjour
broadcast message pour tout le monde
peers
sendfile nodeB ./README.md
leave
quit
```

Les fichiers reçus sont écrits dans `downloads/`.

## Version simple retenue

Cette version cherche le minimum propre pour un projet de L3 :

- `Comm` et `Driver` restent séparés
- socket locale Unix entre les deux
- messages avec en-tête fixe et `payload_len`
- un seul `Comm` connecté à un seul `Driver`
- un seul transfert entrant à la fois
- pas de reprise automatique ni de transferts parallèles

Répartition :

- `Comm`
  - lit les commandes
  - lit et découpe les fichiers
  - affiche les messages reçus
  - écrit les fichiers reçus

- `Driver`
  - gère le jeton
  - gère l'anneau
  - route les messages
  - maintient la topologie
  - livre les événements à `Comm`

## Interface attendue côté Driver

### Socket locale

- type : `AF_UNIX`, `SOCK_STREAM`
- chemin par défaut : `/tmp/anneau_driver.sock`
- le `Driver` ouvre le serveur local, `Comm` s'y connecte

### Framing commun

Chaque message commence par `struct anneau_frame_header` dans [protocol.h](/Users/lilianserre/Desktop/cours/L3/S6/protocoles rÉseaux et communication inter-processus/Projet/include/protocol.h).

Champs importants :

- `magic = 0x41524e47`
- `version = 1`
- `type`
- `request_id`
- `payload_len`

Tous les entiers sont envoyés en ordre réseau.

### Messages envoyés par Comm

- `ANNEAU_MSG_HELLO`
- `ANNEAU_MSG_JOIN_REQ`
- `ANNEAU_MSG_LEAVE_REQ`
- `ANNEAU_MSG_SEND_TEXT_REQ`
- `ANNEAU_MSG_BROADCAST_REQ`
- `ANNEAU_MSG_FILE_START_REQ`
- `ANNEAU_MSG_FILE_CHUNK_REQ`
- `ANNEAU_MSG_FILE_END_REQ`
- `ANNEAU_MSG_TOPOLOGY_REQ`

Détails utiles :

- `JOIN_REQ`
  - `flags == 1` : créer un anneau
  - sinon joindre via `bootstrap_host/bootstrap_port`
- `SEND_TEXT_REQ`
  - payload = `anneau_text_request` puis le texte
- `BROADCAST_REQ`
  - payload = `anneau_broadcast_request` puis le texte
- `FILE_*`
  - `Comm` segmente déjà les fichiers en blocs de `4096` octets

### Messages attendus depuis Driver

- `ANNEAU_MSG_ACK`
- `ANNEAU_MSG_ERROR`
- `ANNEAU_MSG_STATUS_EVT`
- `ANNEAU_MSG_TEXT_EVT`
- `ANNEAU_MSG_BROADCAST_EVT`
- `ANNEAU_MSG_FILE_START_EVT`
- `ANNEAU_MSG_FILE_CHUNK_EVT`
- `ANNEAU_MSG_FILE_END_EVT`
- `ANNEAU_MSG_TOPOLOGY_RSP`

### Contrat minimal pour brancher vite

Pour qu'un vrai `Driver` fonctionne rapidement avec `Comm`, il suffit d'implémenter :

1. le serveur Unix local
2. le parseur et l'émetteur du framing défini dans `protocol.h`
3. `JOIN`, `LEAVE`, `SEND_TEXT`, `BROADCAST`, `TOPOLOGY_REQ`
4. les événements `STATUS_EVT`, `TEXT_EVT`, `BROADCAST_EVT`, `TOPOLOGY_RSP`

Le transfert de fichier peut être branché juste après avec `FILE_START`, `FILE_CHUNK`, `FILE_END`.
