#include "protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_QUEUE 100
#define TOKEN_TIMEOUT_SEC 3

// Structure pour encapsuler une trame en attente d'émission
struct queued_msg {
    struct anneau_frame frame;
};

// Structure globale représentant l'état du programme Driver
struct driver_state {
    char node_id[ANNEAU_MAX_NODE_ID];          // L'identifiant de ce noeud (ex: Machine1)
    char listen_host[ANNEAU_MAX_HOST];          // L'adresse IP ou nom d'hôte sur lequel on écoute
    uint16_t listen_port;                       // Le port TCP d'écoute pour l'anneau

    // Sockets pour la communication interne (avec le processus Comm)
    int localsock_server;                          // Le socket Serveur UNIX (/tmp/anneau_driver.sock)
    int localsock_client;                            // Le socket lié au processus Comm une fois connecté
    
    // Sockets pour la communication sur l'anneau (TCP)
    int tcpsock_server;                            // Le socket Serveur TCP pour accepter le voisin de gauche
    int anneausockg;  
    int anneausockd;   

    // Gestion de la topologie
    struct anneau_peer_info topology[ANNEAU_MAX_TOPOLOGY_ENTRIES];
    uint32_t nb_nodes;                         // Nombre de machines actuellement dans l'anneau
    
    // Gestion du jeton
    bool is_creator;                          // Vrai si on est la 1ère machine (gère la régénération du jeton)
    bool has_token;                         // Vrai si le jeton est actuellement dans notre possession
    time_t last_token_time;                  // Timestamp pour le timeout du jeton

    // File d'attente des messages provenant de Comm
    struct queued_msg queue[MAX_QUEUE];
    int q_head;                              // Indice de lecture de la file
    int q_tail;                              // Indice d'écriture de la file
    int q_size;                            // Nombre d'éléments actuellement dans la file

    // Suivi minimal d'un transfert entrant pour router les chunks au bon Comm
    bool file_receive_active;
    uint32_t file_receive_id;
    bool ignore_next_left_disconnect;
    bool should_exit;
};

/* Initialise la structure du driver à zéro */
static void initialiser_etat(struct driver_state *state) {
    memset(state, 0, sizeof(*state));
    state->localsock_server = -1;
    state->localsock_client = -1;
    state->tcpsock_server = -1;
    state->anneausockg = -1;
    state->anneausockd = -1;
    state->last_token_time = time(NULL);
}

static void recompute_topology_ports(struct driver_state *state) {
    if (state->nb_nodes == 0) {
        return;
    }

    for (uint32_t i = 0; i < state->nb_nodes; i++) {
        uint32_t next = (i + 1) % state->nb_nodes;
        if (state->nb_nodes == 1) {
            state->topology[i].output_port = state->topology[i].input_port;
        } else {
            state->topology[i].output_port = state->topology[next].input_port;
        }
    }
}

static void remove_peer_at(struct driver_state *state, uint32_t index) {
    if (index >= state->nb_nodes) {
        return;
    }

    for (uint32_t i = index; i + 1 < state->nb_nodes; i++) {
        state->topology[i] = state->topology[i + 1];
    }

    if (state->nb_nodes > 0) {
        state->nb_nodes -= 1;
    }
    recompute_topology_ports(state);
}

/* 
 * Ajoute un message à la file d'attente. 
 * Ce message sera conservé en mémoire jusqu'à ce que le Driver reçoive le Jeton.
 */
static void enqueue(struct driver_state *state, const struct anneau_frame *f) {
    if (state->q_size >= MAX_QUEUE) {
        fprintf(stderr, "[Erreur] La file d'attente du driver est pleine. Trame %d ignorée.\n", f->header.type);
        return;
    }
    struct anneau_frame *n_tram = &state->queue[state->q_tail].frame;
    n_tram->header = f->header;
    
    n_tram->payload = malloc(f->header.payload_len);
    if (n_tram->payload && f->header.payload_len > 0) {
        memcpy(n_tram->payload, f->payload, f->header.payload_len);
    }
    state->q_tail = (state->q_tail + 1) % MAX_QUEUE;
    state->q_size++;
}

/*
 * Récupère le message le plus ancien de la file d'attente (FIFO).
 */
static bool dequeue(struct driver_state *state, struct anneau_frame *f) {
    if (state->q_size == 0) return false;
    *f = state->queue[state->q_head].frame;
    state->q_head = (state->q_head + 1) % MAX_QUEUE;
    state->q_size--;
    return true;
}

/* Envoie une trame de données brutes vers le processus Comm */
static void send_to_comm(struct driver_state *state, uint16_t type, const void *payload, uint32_t len) {
    if (state->localsock_client != -1) {
        anneau_send_frame(state->localsock_client, type, 0, payload, len);
    }
}

/* Envoie un statut/notification textuelle vers Comm pour informer l'utilisateur */
static void send_status_to_comm(struct driver_state *state, int code, const char *msg) {
    struct anneau_status_payload s;
    memset(&s, 0, sizeof(s));
    s.code = code;
    anneau_copy_field(s.message, sizeof(s.message), msg);
    send_to_comm(state, ANNEAU_MSG_STATUS_EVT, &s, sizeof(s));
}

/* 
 * Envoie une trame sur l'anneau via le voisin de droite (anneausockd).
 * Si nous sommes la seule machine du réseau, on l'ignore silencieusement.
 */
static void send_to_ring(struct driver_state *state, const struct anneau_frame *f) {
    if (state->nb_nodes <= 1) { 
        return; // Anneau composé uniquement de nous-même
    }
    if (state->anneausockd != -1) {
        anneau_send_frame(state->anneausockd, f->header.type, f->header.request_id, f->payload, f->header.payload_len);
    }
}

static void send_queued_messages_if_token(struct driver_state *state) {
    if (!state->has_token) {
        return;
    }

    struct anneau_frame qf;
    while (dequeue(state, &qf)) {
        if (state->nb_nodes <= 1) {
            send_to_comm(state, qf.header.type, qf.payload, qf.header.payload_len);
        } else {
            send_to_ring(state, &qf);
        }
        anneau_free_frame(&qf);
    }
}

/* Fait circuler le jeton à la machine suivante (la droite) */
static void forward_token(struct driver_state *state) {
    state->has_token = false;
    if (state->nb_nodes > 1 && state->anneausockd != -1) {
        anneau_send_frame(state->anneausockd, ANNEAU_MSG_RING_TOKEN, 0, NULL, 0);
    } else {
        // Si nous sommes seuls, on "garde" implicitement le jeton tout le temps
        state->has_token = true; 
    }
}

/* 
 * Recherche l'index de cette machine dans le tableau de topologie. 
 * Retourne -1 si introuvable (ce qui serait une erreur).
 */
static int get_my_index(struct driver_state *state) {
    for (uint32_t i = 0; i < state->nb_nodes; i++) {
        if (strcmp(state->topology[i].node_id, state->node_id) == 0) {
            return i;
        }
    }
    return -1;
}

/*
 * Reconnecte anneausockd à la machine suivante dans la topologie.
 * Appelée lorsqu'on détecte une coupure du lien droit, ou qu'une nouvelle
 * machine s'est insérée juste après nous.
 */
static void reconnect_right(struct driver_state *state) {
    if (state->anneausockd != -1) {
        close(state->anneausockd);
        state->anneausockd = -1;
    }
    int my_idx = get_my_index(state);
    if (my_idx == -1 || state->nb_nodes <= 1) {
        return; // Rien à faire, ou on est seuls
    }
    
    // Le suivant logique dans le tableau de l'anneau
    int next_idx = (my_idx + 1) % state->nb_nodes;
    struct anneau_peer_info *next = &state->topology[next_idx];

    printf("[driver] Reconnexion de anneausockd (droite) vers %s (%s:%d)\n", next->node_id, next->host, next->input_port);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(next->input_port);
    inet_pton(AF_INET, next->host, &addr.sin_addr);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        state->anneausockd = s;
        printf("[driver] Reconnecté avec succès à %s.\n", next->node_id);
    } else {
        printf("[driver] Echec de la reconnexion à %s.\n", next->node_id);
        close(s);
    }
}

/* Notifie le processus Comm que la liste des machines (topologie) a changée. */
static void topology_handler(struct driver_state *state) {
    if (state->localsock_client != -1) {
        struct anneau_topology_response rsp;
        rsp.count = state->nb_nodes;
        uint32_t payload_len = sizeof(rsp) + state->nb_nodes * sizeof(struct anneau_peer_info);
        uint8_t *payload = malloc(payload_len);
        memcpy(payload, &rsp, sizeof(rsp));
        memcpy(payload + sizeof(rsp), state->topology, state->nb_nodes * sizeof(struct anneau_peer_info));
        send_to_comm(state, ANNEAU_MSG_TOPOLOGY_RSP, payload, payload_len);
        free(payload);
    }
}

static void broadcast_topology(struct driver_state *state) {
    struct anneau_topology_response rsp;
    uint32_t payload_len;
    uint8_t *payload;

    if (state->nb_nodes <= 1 || state->anneausockd == -1) {
        return;
    }

    rsp.count = state->nb_nodes;
    payload_len = sizeof(rsp) + state->nb_nodes * sizeof(struct anneau_peer_info);
    payload = malloc(payload_len);
    if (payload == NULL) {
        return;
    }

    memcpy(payload, &rsp, sizeof(rsp));
    memcpy(payload + sizeof(rsp), state->topology, state->nb_nodes * sizeof(struct anneau_peer_info));
    anneau_send_frame(state->anneausockd, ANNEAU_MSG_TOPOLOGY_RSP, 0, payload, payload_len);
    free(payload);
}

/*
 * Traitement de la commande '/join' reçue depuis Comm.
 * C'est le point d'entrée pour la connexion de ce driver à l'anneau.
 */
static void handle_join_request(struct driver_state *state, const struct anneau_frame *f) {
    const struct anneau_join_request *req = (const struct anneau_join_request*)f->payload;
    
    anneau_copy_field(state->node_id, sizeof(state->node_id), req->node_id);
    anneau_copy_field(state->listen_host, sizeof(state->listen_host), req->listen_host);
    state->listen_port = req->listen_port;

    // Démarrage du Serveur TCP (La porte d'entrée de notre machine)
    state->tcpsock_server = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(state->tcpsock_server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(state->listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(state->tcpsock_server, (struct sockaddr *)&addr, sizeof(addr));
    listen(state->tcpsock_server, 5);

    printf("[driver] En écoute pour l'anneau sur le port %d\n", state->listen_port);

    // Initialisation de la topologie avec nous-même au centre
    state->topology[0].state = 1;
    state->topology[0].input_port = state->listen_port;
    anneau_copy_field(state->topology[0].node_id, sizeof(state->topology[0].node_id), state->node_id);
    anneau_copy_field(state->topology[0].host, sizeof(state->topology[0].host), state->listen_host);
    state->nb_nodes = 1;
    recompute_topology_ports(state);

    // S'il n'y a pas d'hôte bootstrap, nous créons un nouvel anneau (flags = 1)
    if (req->flags == 1) { 
        state->is_creator = true;
        state->has_token = true; // On a le jeton puisqu'on est seul
        state->last_token_time = time(NULL);
        printf("[driver] Anneau créé. Je suis le nœud racine.\n");
        send_status_to_comm(state, 0, "Anneau cree");
        topology_handler(state);
    } else { 
        // Connexion à un anneau existant (Bootstrap)
        state->is_creator = false;
        state->has_token = false;
        
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in baddr;
        memset(&baddr, 0, sizeof(baddr));
        baddr.sin_family = AF_INET;
        baddr.sin_port = htons(req->bootstrap_port);
        inet_pton(AF_INET, req->bootstrap_host, &baddr.sin_addr);

        if (connect(s, (struct sockaddr *)&baddr, sizeof(baddr)) == 0) {
            // On se connecte temporairement au bootstrap par la droite pour envoyer notre demande
            state->anneausockd = s; 

            anneau_send_frame(state->anneausockd, ANNEAU_MSG_JOIN_REQ, 0, req, sizeof(*req));
            send_status_to_comm(state, 0, "Requete de join envoyee");
        } else {
            send_status_to_comm(state, 1, "Echec de connexion à l'hôte bootstrap");
        }
    }
}

/* 
 * Traitement principal du routage et des événements réseau sur l'anneau.
 * Cette fonction lit tout ce qui provient du voisin de gauche (anneausockg).
 */
static void process_ring_frame(struct driver_state *state, struct anneau_frame *f) {
    // 1. GESTION DU JETON
    if (f->header.type == ANNEAU_MSG_RING_TOKEN) {
        state->has_token = true;
        state->last_token_time = time(NULL);

        // Puisque nous avons le droit d'émettre, on vide toute notre file d'attente
        send_queued_messages_if_token(state);
        
        // Et on redonne le jeton au voisin suivant immédiatement
        forward_token(state);
        return;
    }

    // 2. GESTION DES REQUETES DE JONCTION (Une nouvelle machine s'annonce)
    if (f->header.type == ANNEAU_MSG_JOIN_REQ) {
        const struct anneau_join_request *req = (const struct anneau_join_request*)f->payload;
        
        // Vérifie si l'on connait déjà ce nœud pour éviter une boucle infinie
        bool found = false;
        for (uint32_t i = 0; i < state->nb_nodes; i++) {
            if (strcmp(state->topology[i].node_id, req->node_id) == 0) {
                found = true; break;
            }
        }
        
        if (!found && state->nb_nodes < ANNEAU_MAX_TOPOLOGY_ENTRIES) {
            // Ajout du noeud à la fin de notre topologie
            struct anneau_peer_info *pair = &state->topology[state->nb_nodes++];
            pair->state = 1;
            pair->input_port = req->listen_port;
            anneau_copy_field(pair->node_id, sizeof(pair->node_id), req->node_id);
            anneau_copy_field(pair->host, sizeof(pair->host), req->listen_host);
            recompute_topology_ports(state);
            
            topology_handler(state);
            
            // Relais de la nouvelle machine au reste de l'anneau pour que tous la connaissent
            if (strcmp(state->node_id, req->node_id) != 0) {
                send_to_ring(state, f);
            }

            /* Logique de reconfiguration physique :
             * La machine vient de s'insérer en fin de tableau.
             * Si nous étions l'avant-dernière machine, c'est à NOUS de nous connecter 
             * physiquement à elle en reconfigurant notre prise `anneausockd`.
             */
            int my_idx = get_my_index(state);
            if (my_idx != -1 && my_idx == (int)state->nb_nodes - 2) {
                state->ignore_next_left_disconnect = true;
                reconnect_right(state);
            }
            
            /*
             * Si on est le Bootstrap (index 0), il faut envoyer à la nouvelle machine 
             * (qui est actuellement à notre droite) la topologie complète pour 
             * qu'elle connaisse l'ensemble du réseau.
             */
            if (my_idx == 0) { 
                struct anneau_topology_response rsp;
                rsp.count = state->nb_nodes;
                uint32_t payload_len = sizeof(rsp) + state->nb_nodes * sizeof(struct anneau_peer_info);
                uint8_t *payload = malloc(payload_len);
                memcpy(payload, &rsp, sizeof(rsp));
                memcpy(payload + sizeof(rsp), state->topology, state->nb_nodes * sizeof(struct anneau_peer_info));
                
                struct anneau_frame top_f;
                top_f.header.type = ANNEAU_MSG_TOPOLOGY_RSP;
                top_f.header.payload_len = payload_len;
                top_f.header.request_id = 0;
                top_f.payload = payload;
                if (state->anneausockd != -1) {
                    anneau_send_frame(state->anneausockd, top_f.header.type, top_f.header.request_id, top_f.payload, top_f.header.payload_len);
                }
                free(payload);

                if (state->has_token) {
                    forward_token(state);
                }
            }
        }
        return;
    }

    // 3. SYNCHRONISATION DE LA TOPOLOGIE (Réponse du bootstrap)
    if (f->header.type == ANNEAU_MSG_TOPOLOGY_RSP) {
        const struct anneau_topology_response *rsp = (const struct anneau_topology_response*)f->payload;
        if (state->nb_nodes <= 1) {
            // Cas : Nous sommes le petit nouveau et nous recevons l'image globale du réseau
            state->nb_nodes = rsp->count;
            memcpy(state->topology, f->payload + sizeof(*rsp), rsp->count * sizeof(struct anneau_peer_info));
            recompute_topology_ports(state);
            printf("[driver] Topologie synchronisée, %u noeuds présents.\n", state->nb_nodes);
            topology_handler(state);
            
            // On connecte notre "droite" au Nœud 0 (Boucler l'anneau)
            reconnect_right(state);
        } else {
             // Cas : Mise à jour en cascade
             if (rsp->count != state->nb_nodes) {
                 state->nb_nodes = rsp->count;
                 memcpy(state->topology, f->payload + sizeof(*rsp), rsp->count * sizeof(struct anneau_peer_info));
                 recompute_topology_ports(state);
                 topology_handler(state);
                 send_to_ring(state, f);
             }
        }
        return;
    }

    // 4. ROUTAGE DES DONNEES (Messages, Fichiers, Diffusion)
    bool for_me = false;
    if (f->header.type == ANNEAU_MSG_TEXT_EVT) {
        const struct anneau_text_event *evt = (const struct anneau_text_event*)f->payload;
        if (strcmp(evt->destination, state->node_id) == 0) {
            for_me = true;
            send_to_comm(state, f->header.type, f->payload, f->header.payload_len);
        }
        // Si je ne suis pas l'émetteur d'origine, je fais circuler la trame
        if (strcmp(evt->source, state->node_id) != 0) {
            if (!for_me) send_to_ring(state, f);
        }
    } else if (f->header.type == ANNEAU_MSG_BROADCAST_EVT) {
        const struct anneau_broadcast_event *evt = (const struct anneau_broadcast_event*)f->payload;
        if (strcmp(evt->source, state->node_id) != 0) {
            send_to_comm(state, f->header.type, f->payload, f->header.payload_len);
            send_to_ring(state, f); // Transmet toujours au suivant !
        }
    } else if (f->header.type == ANNEAU_MSG_FILE_START_EVT) {
        const struct anneau_file_start *req = (const struct anneau_file_start *)f->payload;
        if (strcmp(req->peer_id, state->node_id) == 0) {
            state->file_receive_active = true;
            state->file_receive_id = req->transfer_id;
            send_to_comm(state, f->header.type, f->payload, f->header.payload_len);
        } else {
            send_to_ring(state, f);
        }
    } else if (f->header.type == ANNEAU_MSG_FILE_CHUNK_EVT) {
        const struct anneau_file_chunk *chunk = (const struct anneau_file_chunk *)f->payload;
        if (state->file_receive_active && chunk->transfer_id == state->file_receive_id) {
            send_to_comm(state, f->header.type, f->payload, f->header.payload_len);
        } else {
            send_to_ring(state, f);
        }
    } else if (f->header.type == ANNEAU_MSG_FILE_END_EVT) {
        const struct anneau_file_end *end = (const struct anneau_file_end *)f->payload;
        if (state->file_receive_active && end->transfer_id == state->file_receive_id) {
            send_to_comm(state, f->header.type, f->payload, f->header.payload_len);
            state->file_receive_active = false;
            state->file_receive_id = 0;
        } else {
            send_to_ring(state, f);
        }
    } else {
        // Trame inconnue ou morceaux de fichiers : on transfère bêtement
        send_to_ring(state, f);
    }
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    struct driver_state state;
    initialiser_etat(&state);

    // Initialisation et création du serveur socket Unix pour communiquer avec `Comm`
    const char *chemin_sock = ANNEAU_DEFAULT_SOCKET_PATH;
    unlink(chemin_sock); // Sécurité : On s'assure qu'aucun ancien fichier mort n'existe

    state.localsock_server = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un un_addr;
    memset(&un_addr, 0, sizeof(un_addr));
    un_addr.sun_family = AF_UNIX;
    strncpy(un_addr.sun_path, chemin_sock, sizeof(un_addr.sun_path)-1);
    bind(state.localsock_server, (struct sockaddr *)&un_addr, sizeof(un_addr));
    listen(state.localsock_server, 1);

    printf("Processus Driver démarré, écoute du Comm sur : %s\n", chemin_sock);

    // Boucle d'événements principale (Event Loop) asynchrone
    while (1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        int max_fd = -1;

        // On surveille le socket local
        FD_SET(state.localsock_server, &readfds);
        if (state.localsock_server > max_fd) max_fd = state.localsock_server;

        // On surveille Comm s'il est connecté
        if (state.localsock_client != -1) {
            FD_SET(state.localsock_client, &readfds);
            if (state.localsock_client > max_fd) max_fd = state.localsock_client;
        }

        // On surveille les nouvelles connexions TCP sur l'anneau (nouvelle machine entrante)
        if (state.tcpsock_server != -1) {
            FD_SET(state.tcpsock_server, &readfds);
            if (state.tcpsock_server > max_fd) max_fd = state.tcpsock_server;
        }

        // On surveille les données arrivant de notre voisin gauche
        if (state.anneausockg != -1) {
            FD_SET(state.anneausockg, &readfds);
            if (state.anneausockg > max_fd) max_fd = state.anneausockg;
        }

        // Timeout d'1 seconde pour permettre l'exécution des tâches de fond (coeur battant du token)
        struct timeval tv;
        tv.tv_sec = 1; 
        tv.tv_usec = 0;

        int ret = select(max_fd + 1, &readfds, NULL, NULL, &tv);

        if (ret < 0) {
            if (errno == EINTR) continue; // Interrompu par un signal, on relance
            perror("Erreur critique sur select");
            break;
        }

        // Vérification de la perte du jeton
        if (state.is_creator && state.nb_nodes > 1) {
            if (!state.has_token && (time(NULL) - state.last_token_time) > TOKEN_TIMEOUT_SEC) {
                printf("[driver] ATTENTION: Jeton perdu ! Régénération du jeton.\n");
                state.has_token = true;
                state.last_token_time = time(NULL);
                forward_token(&state);
            }
        } else if (state.has_token && state.nb_nodes > 1) {
            // Si on détient le jeton virtuellement mais qu'il n'y a pas d'activité, on le passe au suivant
            forward_token(&state);
        }

        if (ret == 0) continue; // Fin du délai timeout du select(), on reboucle

        // 1. Nouvelle connexion depuis l'outil IHM (Comm)
        if (FD_ISSET(state.localsock_server, &readfds)) {
            int c = accept(state.localsock_server, NULL, NULL);
            if (c >= 0) {
                if (state.localsock_client != -1) close(state.localsock_client);
                state.localsock_client = c;
                printf("[driver] IHM Comm connectée.\n");
            }
        }

        // 2. Réception d'une commande / requête issue de l'IHM
        if (state.localsock_client != -1 && FD_ISSET(state.localsock_client, &readfds)) {
            struct anneau_frame f;
            if (anneau_recv_frame(state.localsock_client, &f) > 0) {
                
                if (f.header.type == ANNEAU_MSG_JOIN_REQ) {
                    handle_join_request(&state, &f);
                } 
                else if (f.header.type == ANNEAU_MSG_LEAVE_REQ) {
                    printf("[driver] Comm demande de quitter. Arrêt gracieux.\n");
                    close(state.localsock_client);
                    state.localsock_client = -1;
                    state.should_exit = true;
                } 
                else if (f.header.type == ANNEAU_MSG_TOPOLOGY_REQ) {
                    topology_handler(&state);
                } 
                else if (f.header.type >= 10 && f.header.type < 20) {
                    /*
                     * Toute requête de données (10-19) est transformée en événement (30-39)
                     * puis placée dans la file d'attente pour être envoyée quand on a le jeton.
                     */
                    struct anneau_frame frame_evt;
                    frame_evt.header = f.header;
                    
                    if (f.header.type == ANNEAU_MSG_SEND_TEXT_REQ) {
                        frame_evt.header.type = ANNEAU_MSG_TEXT_EVT;
                        const struct anneau_text_request *req = (const struct anneau_text_request *)f.payload;
                        struct anneau_text_event *evt = malloc(sizeof(*evt) + req->text_len);
                        anneau_copy_field(evt->source, sizeof(evt->source), state.node_id);
                        anneau_copy_field(evt->destination, sizeof(evt->destination), req->destination);
                        evt->text_len = req->text_len;
                        memcpy((uint8_t*)evt + sizeof(*evt), f.payload + sizeof(*req), req->text_len);
                        frame_evt.header.payload_len = sizeof(*evt) + req->text_len;
                        frame_evt.payload = (uint8_t*)evt;
                        enqueue(&state, &frame_evt);
                        send_queued_messages_if_token(&state);
                        free(evt);
                    } 
                    else if (f.header.type == ANNEAU_MSG_BROADCAST_REQ) {
                        frame_evt.header.type = ANNEAU_MSG_BROADCAST_EVT;
                        const struct anneau_broadcast_request *req = (const struct anneau_broadcast_request *)f.payload;
                        struct anneau_broadcast_event *evt = malloc(sizeof(*evt) + req->text_len);
                        anneau_copy_field(evt->source, sizeof(evt->source), state.node_id);
                        evt->text_len = req->text_len;
                        memcpy((uint8_t*)evt + sizeof(*evt), f.payload + sizeof(*req), req->text_len);
                        frame_evt.header.payload_len = sizeof(*evt) + req->text_len;
                        frame_evt.payload = (uint8_t*)evt;
                        enqueue(&state, &frame_evt);
                        send_queued_messages_if_token(&state);
                        free(evt);
                    } 
                    else {
                        // Cas des fichiers (FILE_START, FILE_CHUNK, FILE_END)
                        // On se contente de changer les flags pour qu'ils soient identifiés comme Events sur le réseau
                        if (f.header.type == ANNEAU_MSG_FILE_START_REQ) frame_evt.header.type = ANNEAU_MSG_FILE_START_EVT;
                        if (f.header.type == ANNEAU_MSG_FILE_CHUNK_REQ) frame_evt.header.type = ANNEAU_MSG_FILE_CHUNK_EVT;
                        if (f.header.type == ANNEAU_MSG_FILE_END_REQ)   frame_evt.header.type = ANNEAU_MSG_FILE_END_EVT;
                        frame_evt.payload = f.payload;
                        enqueue(&state, &frame_evt);
                        send_queued_messages_if_token(&state);
                    }
                }
                anneau_free_frame(&f);
            } else {
                printf("[driver] IHM Comm déconnectée brutalement.\n");
                close(state.localsock_client);
                state.localsock_client = -1;
                state.should_exit = true;
            }
        }

        // 3. Réception d'une nouvelle machine voulant se lier à notre gauche (Serveur TCP)
        if (state.tcpsock_server != -1 && FD_ISSET(state.tcpsock_server, &readfds)) {
            int c = accept(state.tcpsock_server, NULL, NULL);
            if (c >= 0) {
                // Si quelqu'un était déjà branché à notre gauche, on le coupe (il réagira)
                if (state.anneausockg != -1) close(state.anneausockg);
                state.anneausockg = c;
                printf("[driver] Nouvelle connexion TCP reçue sur la 'gauche' (anneausockg)\n");
            }
        }

        // 4. Flux de données en provenance de la machine de gauche (L'anneau qui tourne)
        if (state.anneausockg != -1 && FD_ISSET(state.anneausockg, &readfds)) {
            struct anneau_frame f;
            int r = anneau_recv_frame(state.anneausockg, &f);
            if (r > 0) {
                process_ring_frame(&state, &f);
                anneau_free_frame(&f);
            } else {
                // Détection de déconnexion ! La machine de gauche a crashée ou nous a remplacés.
                printf("[driver] La machine voisine (Gauche) s'est déconnectée.\n");
                close(state.anneausockg);
                state.anneausockg = -1;

                if (state.ignore_next_left_disconnect) {
                    state.ignore_next_left_disconnect = false;
                } else if (state.nb_nodes > 1) {
                    int my_idx = get_my_index(&state);
                    if (my_idx != -1) {
                        uint32_t left_idx = (uint32_t)((my_idx + (int)state.nb_nodes - 1) % (int)state.nb_nodes);
                        printf("[driver] Retrait du voisin gauche %s de la topologie.\n",
                               state.topology[left_idx].node_id);
                        remove_peer_at(&state, left_idx);
                        topology_handler(&state);
                        if (state.nb_nodes > 1) {
                            reconnect_right(&state);
                            if (state.is_creator) {
                                broadcast_topology(&state);
                            }
                        }
                    }
                }
            }
        }

        if (state.should_exit) {
            break;
        }
    }

    if (state.localsock_client != -1) close(state.localsock_client);
    if (state.localsock_server != -1) close(state.localsock_server);
    if (state.anneausockg != -1) close(state.anneausockg);
    if (state.anneausockd != -1) close(state.anneausockd);
    if (state.tcpsock_server != -1) close(state.tcpsock_server);
    unlink(chemin_sock);
    return 0;
}
