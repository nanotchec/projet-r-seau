# Interface Driver pour la partie Comm

Cette note est volontairement courte. Elle décrit exactement ce que `Comm` attend du `Driver` pour que vous puissiez avancer en parallèle.

## Socket locale

- Type : `AF_UNIX`, `SOCK_STREAM`
- Chemin par défaut : `/tmp/anneau_driver.sock`
- Le `Driver` ouvre le serveur local, `Comm` s'y connecte comme client.

## Framing commun

Chaque message commence par `struct anneau_frame_header` définie dans [protocol.h](/Users/lilianserre/Desktop/cours/L3/S6/protocoles rÉseaux et communication inter-processus/Projet/include/protocol.h).

Champs :

- `magic = 0x41524e47`
- `version = 1`
- `type`
- `request_id`
- `payload_len`

Tous les entiers sont envoyés en ordre réseau. Le `Driver` peut réutiliser ce même framing sur l'anneau si ça vous simplifie la vie.

## Messages envoyés par Comm

- `ANNEAU_MSG_HELLO`
  - payload `anneau_status_payload`
  - sert juste à vérifier que la socket locale répond
- `ANNEAU_MSG_JOIN_REQ`
  - payload `anneau_join_request`
  - `flags == 1` : créer un nouvel anneau localement
  - sinon joindre via `bootstrap_host/bootstrap_port`
- `ANNEAU_MSG_LEAVE_REQ`
  - payload `anneau_leave_request`
- `ANNEAU_MSG_SEND_TEXT_REQ`
  - payload = `anneau_text_request` puis `text_len` octets
- `ANNEAU_MSG_BROADCAST_REQ`
  - payload = `anneau_broadcast_request` puis `text_len` octets
- `ANNEAU_MSG_FILE_START_REQ`
  - payload `anneau_file_start`
  - `peer_id` = destination
- `ANNEAU_MSG_FILE_CHUNK_REQ`
  - payload = `anneau_file_chunk` puis `data_len` octets
- `ANNEAU_MSG_FILE_END_REQ`
  - payload `anneau_file_end`
- `ANNEAU_MSG_TOPOLOGY_REQ`
  - payload `anneau_topology_request`

Important :

- `Comm` segmente déjà les fichiers en blocs de `4096` octets.
- `Driver` n'a pas besoin de lire les fichiers sur disque, seulement de transporter les messages.
- `request_id` permet de rattacher un `ACK` ou une `ERROR` à la requête locale.

## Messages attendus depuis Driver

- `ANNEAU_MSG_ACK`
  - payload `anneau_status_payload`
- `ANNEAU_MSG_ERROR`
  - payload `anneau_status_payload`
- `ANNEAU_MSG_STATUS_EVT`
  - payload `anneau_status_payload`
  - utile pour notifier `join`, `leave`, régénération de jeton, erreur réseau, etc.
- `ANNEAU_MSG_TEXT_EVT`
  - payload = `anneau_text_event` puis `text_len` octets
  - à utiliser quand un message unicast est délivré à la machine locale
- `ANNEAU_MSG_BROADCAST_EVT`
  - payload = `anneau_broadcast_event` puis `text_len` octets
- `ANNEAU_MSG_FILE_START_EVT`
  - payload `anneau_file_start`
  - `peer_id` = source du fichier
- `ANNEAU_MSG_FILE_CHUNK_EVT`
  - payload = `anneau_file_chunk` puis `data_len` octets
- `ANNEAU_MSG_FILE_END_EVT`
  - payload `anneau_file_end`
- `ANNEAU_MSG_TOPOLOGY_RSP`
  - payload = `anneau_topology_response` puis `count` entrées `anneau_peer_info`

## Répartition des rôles

- `Comm`
  - menu utilisateur
  - validation simple des entrées
  - lecture du fichier à envoyer
  - écriture du fichier reçu
  - affichage des messages et de la topologie

- `Driver`
  - sockets anneau gauche/droite
  - gestion du jeton
  - routage des messages
  - injection dans l'anneau
  - réception depuis l'anneau et livraison vers `Comm`
  - maintien des infos topologie
  - régénération du jeton si besoin

## Contrat minimal pour brancher vite

Pour que `Comm` marche immédiatement avec un vrai `Driver`, il suffit d'implémenter côté `Driver` :

1. le serveur Unix local ;
2. le parseur/émetteur du framing défini dans `protocol.h` ;
3. `JOIN`, `LEAVE`, `SEND_TEXT`, `BROADCAST`, `TOPOLOGY_REQ` ;
4. les événements `STATUS_EVT`, `TEXT_EVT`, `BROADCAST_EVT`, `TOPOLOGY_RSP`.

Le transfert de fichier peut être branché juste après avec les trois messages `FILE_START/CHUNK/END`.
