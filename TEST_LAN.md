# Test LAN rapide

Ce document explique comment lancer et tester le projet sur plusieurs machines d'un meme reseau local.

## Prerequis

Sur chaque machine :

```sh
make
```

Verifier aussi :

- que les machines se pingent entre elles
- que les ports choisis sont libres
- qu'aucun pare-feu ne bloque les connexions TCP entre les machines

## Programmes a lancer

Sur chaque machine, il faut lancer :

1. `driver`
2. `comm`

Exemple :

Terminal 1 :

```sh
./build/bin/driver
```

Terminal 2 :

```sh
./build/bin/comm
```

## Commande de base

Dans `comm` :

```text
join <id_noeud> <host_ecoute> <port_ecoute> [host_bootstrap port_bootstrap]
```

Regles :

- sans `host_bootstrap port_bootstrap` : la machine cree l'anneau
- avec `host_bootstrap port_bootstrap` : la machine rejoint un anneau existant

## Test a 2 machines

Exemple :

- Machine A : `192.168.1.10`
- Machine B : `192.168.1.11`

### Machine A

Lancer :

```sh
./build/bin/driver
./build/bin/comm
```

Puis dans `comm` :

```text
join nodeA 192.168.1.10 9000
peers
```

### Machine B

Lancer :

```sh
./build/bin/driver
./build/bin/comm
```

Puis dans `comm` :

```text
join nodeB 192.168.1.11 9100 192.168.1.10 9000
peers
```

### Verification attendue

Sur les deux machines, `peers` doit finir par afficher :

- `nodeA`
- `nodeB`

## Test des messages

Depuis la machine B :

```text
send nodeA bonjour depuis B
broadcast message pour tout le monde
```

Verification attendue sur la machine A :

- reception du message unicast
- reception de la diffusion

## Test de fichier

Depuis la machine B :

```text
sendfile nodeA ./README.md
```

Verification attendue sur la machine A :

- creation d'un fichier dans `downloads/`

## Test a 3 machines

Exemple :

- Machine A : `192.168.1.10`
- Machine B : `192.168.1.11`
- Machine C : `192.168.1.12`

Ordre conseille :

1. A cree l'anneau
2. B rejoint A
3. C rejoint A

Commandes :

### Machine A

```text
join nodeA 192.168.1.10 9000
```

### Machine B

```text
join nodeB 192.168.1.11 9100 192.168.1.10 9000
```

### Machine C

```text
join nodeC 192.168.1.12 9200 192.168.1.10 9000
```

Puis sur chaque machine :

```text
peers
```

Verification attendue :

- les trois noeuds doivent apparaitre

## En cas de probleme

Verifier dans cet ordre :

1. ping entre les machines
2. bonne IP dans la commande `join`
3. bon port d'ecoute du bootstrap
4. `driver` bien lance avant `comm`
5. aucun ancien socket local bloque :

```sh
rm -f /tmp/anneau_driver.sock
```

6. recompilation propre :

```sh
make clean && make
```

## Limites connues

Cette base est suffisante pour des tests simples, mais elle n'est pas encore validee comme implementation complete et robuste de tout le sujet.

En particulier, il faut rester prudent sur :

- la fiabilite a plus de 2 noeuds
- les departs dynamiques
- la regeneration du jeton
- les transferts de fichiers longs
