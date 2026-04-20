# Projet anneau - partie Comm

Cette base implémente la partie `Comm` du projet en C, avec une socket locale Unix pour parler au `Driver`.

## Contenu

- `build/bin/comm` : interface utilisateur en ligne de commande.
- `build/bin/mock_driver` : faux driver local pour tester l'interface sans attendre l'implémentation anneau.
- `include/protocol.h` : contrat partagé `Comm <-> Driver`.
- `docs/DRIVER_INTERFACE.md` : doc courte à transmettre au binôme côté driver.

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

Exemples de commandes dans `comm` :

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

## Choix faits

- `Comm` reste responsable de l'IHM, de la lecture/écriture des fichiers et du découpage en blocs.
- `Driver` reste responsable du transport, du jeton, de l'anneau, du routage et de la topologie.
- Toutes les trames sont délimitées par un en-tête fixe avec `type`, `request_id` et `payload_len`, conformément au sujet.
