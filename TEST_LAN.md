# Tests LAN pour verifier le projet

Objectif : verifier rapidement que le projet respecte les fonctions principales du PDF sur plusieurs machines du meme reseau.

## 1. Preparation

Sur chaque machine :

```sh
make clean && make
rm -f /tmp/anneau_driver.sock
```

Puis ouvrir deux terminaux par machine.

Terminal 1 :

```sh
./build/bin/driver
```

Terminal 2 :

```sh
./build/bin/comm
```

Verifier les IP avec :

```sh
hostname -I
```

Exemple teste :

- machine A : `nodeA`, IP `192.168.64.1`, port `9000`
- machine B : `nodeB`, IP `192.168.64.7`, port `9100`
- machine C : `nodeC`, IP `192.168.64.6`, port `9200`

## 2. Creer l'anneau

Dans `comm` sur la machine A :

```text
join nodeA 192.168.64.1 9000
```

Resultat attendu :

```text
nodeA 192.168.64.1 9000 9000 1
```

## 3. Ajouter une deuxieme machine

Dans `comm` sur la machine B :

```text
join nodeB 192.168.64.7 9100 192.168.64.1 9000
```

Puis sur A et B :

```text
peers
```

Resultat attendu sur les deux machines :

```text
nodeA 192.168.64.1 9000 9100 1
nodeB 192.168.64.7 9100 9000 1
```

## 4. Ajouter une troisieme machine

Attendre que A et B affichent bien la meme topologie, puis dans `comm` sur la machine C :

```text
join nodeC 192.168.64.6 9200 192.168.64.1 9000
```

Puis sur A, B et C :

```text
peers
```

Resultat attendu sur les trois machines :

```text
nodeA 192.168.64.1 9000 9100 1
nodeB 192.168.64.7 9100 9200 1
nodeC 192.168.64.6 9200 9000 1
```

## 5. Tester les messages

Depuis C vers A :

```text
send nodeA message-C-vers-A
```

A doit afficher :

```text
[message] nodeC -> nodeA : message-C-vers-A
```

Depuis A vers C :

```text
send nodeC message-A-vers-C
```

C doit afficher :

```text
[message] nodeA -> nodeC : message-A-vers-C
```

Depuis B :

```text
broadcast diffusion-B
```

A et C doivent afficher :

```text
[diffusion] nodeB : diffusion-B
```

## 6. Tester un fichier

Sur A, creer un petit fichier :

```sh
printf 'test fichier trois machines\n' > /tmp/test-anneau-3.txt
```

Dans `comm` sur A :

```text
sendfile nodeC /tmp/test-anneau-3.txt
```

Sur C, verifier :

```sh
cat downloads/nodeC_test-anneau-3.txt
```

Resultat attendu :

```text
test fichier trois machines
```

## 7. Tester un depart

Dans `comm` sur C :

```text
leave
```

Puis sur A et B :

```text
peers
```

Resultat attendu :

```text
nodeA 192.168.64.1 9000 9100 1
nodeB 192.168.64.7 9100 9000 1
```

Verifier que l'anneau restant fonctionne encore. Dans `comm` sur B :

```text
send nodeA apres-leave-C-ok
```

A doit afficher :

```text
[message] nodeB -> nodeA : apres-leave-C-ok
```

## 8. Nettoyage

Sur chaque machine :

```sh
pkill -x driver || true
pkill -x comm || true
rm -f /tmp/anneau_driver.sock
```

## Verdict attendu

Si toutes les etapes passent, les fonctions principales du PDF sont validees sur LAN :

- creation de l'anneau
- ajout de machines
- recuperation de la topologie avec port entrant et port sortant
- envoi direct
- diffusion
- transfert de fichier
- depart d'un noeud avec maintien de l'anneau restant

Limites a annoncer si on veut etre parfaitement honnete :

- tests faits jusqu'a 3 machines
- transfert fichier valide sur petit fichier
- pas de test de gros fichier ni de panne brutale type coupure reseau ou crash machine
