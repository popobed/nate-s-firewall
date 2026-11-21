/**
 * Reseau P2P serverless : signalisation WebRTC relayee de pair a pair
 * (aucun serveur de signalisation central une fois la premiere connexion
 * du maillage etablie) et table de routage Kademlia (DHT) sur
 * DataChannels pour la decouverte de pairs en O(log N).
 *
 * WebRTC exige intrinsequement un canal de signalisation hors-bande pour
 * la toute PREMIERE connexion (echange de l'offre/reponse SDP) : il n'y
 * a aucun moyen de contourner cette contrainte du protocole lui-meme.
 * "Sans serveur central" signifie ici qu'aucun service de signalisation
 * permanent n'est requis au-dela de ce premier bootstrap : l'application
 * hote transporte l'offre/reponse initiale par le canal de son choix
 * (lien, QR code, presse-papiers -- hors du perimetre de ce module), et
 * TOUTES les connexions suivantes sont negociees en relayant les
 * messages de signalisation via les DataChannels deja etablis du
 * maillage, route par route, selon la table Kademlia.
 */

// ---------------------------------------------------------------------
// Identifiants de pair et metrique XOR (Kademlia)
// ---------------------------------------------------------------------

export type PeerId = string; // 256 bits, encode en hexadecimal (64 caracteres)

const ID_BITS = 256;
const K_BUCKET_SIZE = 8;
const SIGNAL_RELAY_MAX_TTL = 16;

export function generatePeerId(): PeerId {
  const bytes = new Uint8Array(32);
  crypto.getRandomValues(bytes);
  return Array.from(bytes)
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

function xorDistance(a: PeerId, b: PeerId): bigint {
  return BigInt(`0x${a}`) ^ BigInt(`0x${b}`);
}

function highestSetBitIndex(value: bigint): number {
  if (value === 0n) {
    return -1;
  }
  return value.toString(2).length - 1;
}

// ---------------------------------------------------------------------
// Table de routage Kademlia (k-buckets)
// ---------------------------------------------------------------------

export interface RoutablePeer {
  readonly peerId: PeerId;
  lastSeenAtMs: number;
}

export class KademliaRoutingTable {
  private readonly buckets: RoutablePeer[][];

  constructor(private readonly selfId: PeerId) {
    this.buckets = Array.from({ length: ID_BITS }, () => []);
  }

  private bucketIndexFor(peerId: PeerId): number {
    const idx = highestSetBitIndex(xorDistance(this.selfId, peerId));
    return idx < 0 ? 0 : idx;
  }

  upsert(peerId: PeerId): void {
    if (peerId === this.selfId) {
      return;
    }
    const idx = this.bucketIndexFor(peerId);
    const bucket = this.buckets[idx]!;
    const existingPos = bucket.findIndex((p) => p.peerId === peerId);
    if (existingPos >= 0) {
      bucket.splice(existingPos, 1);
    } else if (bucket.length >= K_BUCKET_SIZE) {
      bucket.shift(); // evince le contact le plus anciennement vu (LRU simplifie)
    }
    bucket.push({ peerId, lastSeenAtMs: Date.now() });
  }

  remove(peerId: PeerId): void {
    const idx = this.bucketIndexFor(peerId);
    this.buckets[idx] = this.buckets[idx]!.filter((p) => p.peerId !== peerId);
  }

  /** Les `count` pairs connus les plus proches de `targetId` (metrique XOR). */
  closestTo(targetId: PeerId, count: number): PeerId[] {
    return this.allPeers()
      .sort((a, b) => {
        const da = xorDistance(a, targetId);
        const db = xorDistance(b, targetId);
        return da < db ? -1 : da > db ? 1 : 0;
      })
      .slice(0, count);
  }

  allPeers(): PeerId[] {
    return this.buckets.flatMap((bucket) => bucket.map((p) => p.peerId));
  }

  size(): number {
    return this.buckets.reduce((sum, bucket) => sum + bucket.length, 0);
  }
}

// ---------------------------------------------------------------------
// Messages de signalisation WebRTC relayes de pair a pair
// ---------------------------------------------------------------------

export type SignalPayload =
  | { kind: "offer"; sdp: RTCSessionDescriptionInit }
  | { kind: "answer"; sdp: RTCSessionDescriptionInit }
  | { kind: "ice-candidate"; candidate: RTCIceCandidateInit };

export interface SignalEnvelope {
  readonly type: "signal-relay";
  readonly fromPeerId: PeerId;
  readonly toPeerId: PeerId;
  readonly ttl: number;
  readonly payload: SignalPayload;
}

export interface DhtFindNodeRequest {
  readonly type: "dht-find-node";
  readonly requestId: string;
  readonly targetId: PeerId;
}

export interface DhtFindNodeResponse {
  readonly type: "dht-find-node-response";
  readonly requestId: string;
  readonly peers: readonly PeerId[];
}

export type ControlMessage = SignalEnvelope | DhtFindNodeRequest | DhtFindNodeResponse;

// ---------------------------------------------------------------------
// Connexion a un pair : une RTCPeerConnection + deux DataChannels
// (`control` pour la signalisation relayee et le DHT, `sync` pour la
// charge utile applicative -- CRDT vectoriel, cf. crdt_sync.ts)
// ---------------------------------------------------------------------

export interface MeshTransportHandlers {
  onControlMessage(fromPeerId: PeerId, message: ControlMessage): void;
  onSyncMessage(fromPeerId: PeerId, data: ArrayBuffer): void;
  onPeerConnected(peerId: PeerId): void;
  onPeerDisconnected(peerId: PeerId): void;
}

const RTC_CONFIGURATION: RTCConfiguration = {
  iceServers: [
    // Serveurs STUN publics uniquement (decouverte d'adresse), aucun
    // serveur d'application/signalisation : conforme a l'exigence
    // "sans serveur central" pour le PLAN DE DONNEES applicatif.
    { urls: ["stun:stun.l.google.com:19302"] },
  ],
};

class PeerLink {
  readonly connection: RTCPeerConnection;
  private controlChannel: RTCDataChannel | null = null;
  private syncChannel: RTCDataChannel | null = null;
  private readonly pendingIceCandidates: RTCIceCandidateInit[] = [];

  constructor(
    readonly remotePeerId: PeerId,
    private readonly handlers: MeshTransportHandlers,
    isInitiator: boolean,
  ) {
    this.connection = new RTCPeerConnection(RTC_CONFIGURATION);
    this.connection.onconnectionstatechange = () => {
      if (this.connection.connectionState === "connected") {
        this.handlers.onPeerConnected(this.remotePeerId);
      } else if (
        this.connection.connectionState === "disconnected" ||
        this.connection.connectionState === "failed" ||
        this.connection.connectionState === "closed"
      ) {
        this.handlers.onPeerDisconnected(this.remotePeerId);
      }
    };

    if (isInitiator) {
      this.controlChannel = this.connection.createDataChannel("control", { ordered: true });
      this.syncChannel = this.connection.createDataChannel("sync", { ordered: true });
      this.wireChannel(this.controlChannel, "control");
      this.wireChannel(this.syncChannel, "sync");
    } else {
      this.connection.ondatachannel = (event) => {
        if (event.channel.label === "control") {
          this.controlChannel = event.channel;
          this.wireChannel(event.channel, "control");
        } else if (event.channel.label === "sync") {
          this.syncChannel = event.channel;
          this.wireChannel(event.channel, "sync");
        }
      };
    }
  }

  private wireChannel(channel: RTCDataChannel, kind: "control" | "sync"): void {
    channel.binaryType = "arraybuffer";
    channel.onmessage = (event: MessageEvent) => {
      if (kind === "control") {
        this.handlers.onControlMessage(this.remotePeerId, JSON.parse(event.data as string) as ControlMessage);
      } else {
        this.handlers.onSyncMessage(this.remotePeerId, event.data as ArrayBuffer);
      }
    };
  }

  async createOffer(): Promise<RTCSessionDescriptionInit> {
    const offer = await this.connection.createOffer();
    await this.connection.setLocalDescription(offer);
    return offer;
  }

  async createAnswer(offer: RTCSessionDescriptionInit): Promise<RTCSessionDescriptionInit> {
    await this.connection.setRemoteDescription(offer);
    const answer = await this.connection.createAnswer();
    await this.connection.setLocalDescription(answer);
    await this.drainPendingIceCandidates();
    return answer;
  }

  async acceptAnswer(answer: RTCSessionDescriptionInit): Promise<void> {
    await this.connection.setRemoteDescription(answer);
    await this.drainPendingIceCandidates();
  }

  async addIceCandidate(candidate: RTCIceCandidateInit): Promise<void> {
    if (!this.connection.remoteDescription) {
      this.pendingIceCandidates.push(candidate);
      return;
    }
    await this.connection.addIceCandidate(candidate);
  }

  private async drainPendingIceCandidates(): Promise<void> {
    while (this.pendingIceCandidates.length > 0) {
      const candidate = this.pendingIceCandidates.shift();
      if (candidate) {
        await this.connection.addIceCandidate(candidate);
      }
    }
  }

  sendControl(message: ControlMessage): void {
    if (this.controlChannel && this.controlChannel.readyState === "open") {
      this.controlChannel.send(JSON.stringify(message));
    }
  }

  sendSync(data: ArrayBuffer): void {
    if (this.syncChannel && this.syncChannel.readyState === "open") {
      this.syncChannel.send(data);
    }
  }

  close(): void {
    this.controlChannel?.close();
    this.syncChannel?.close();
    this.connection.close();
  }
}

// ---------------------------------------------------------------------
// Noeud de maillage : orchestre les PeerLink, le routage Kademlia et le
// relais de signalisation serverless.
// ---------------------------------------------------------------------

export class SynapseMeshNode {
  readonly selfId: PeerId;
  readonly routingTable: KademliaRoutingTable;
  private readonly links = new Map<PeerId, PeerLink>();
  private readonly pendingFindNode = new Map<string, (peers: readonly PeerId[]) => void>();

  constructor(
    private readonly handlers: Pick<MeshTransportHandlers, "onSyncMessage" | "onPeerConnected" | "onPeerDisconnected">,
    selfId: PeerId = generatePeerId(),
  ) {
    this.selfId = selfId;
    this.routingTable = new KademliaRoutingTable(selfId);
  }

  private transportHandlers(): MeshTransportHandlers {
    return {
      onControlMessage: (from, message) => this.handleControlMessage(from, message),
      onSyncMessage: this.handlers.onSyncMessage,
      onPeerConnected: (peerId) => {
        this.routingTable.upsert(peerId);
        this.handlers.onPeerConnected(peerId);
      },
      onPeerDisconnected: (peerId) => {
        this.links.delete(peerId);
        this.routingTable.remove(peerId);
        this.handlers.onPeerDisconnected(peerId);
      },
    };
  }

  // --- Bootstrap hors-bande (premiere connexion du maillage uniquement) ---

  /** Cote initiateur : produit une offre serialisee a transmettre par un canal externe au choix de l'application. */
  async createBootstrapOffer(remotePeerId: PeerId): Promise<string> {
    const link = new PeerLink(remotePeerId, this.transportHandlers(), true);
    this.links.set(remotePeerId, link);
    const offer = await link.createOffer();
    return JSON.stringify({ fromPeerId: this.selfId, sdp: offer });
  }

  /** Cote recepteur : consomme l'offre externe, produit une reponse serialisee a renvoyer par le meme canal externe. */
  async acceptBootstrapOffer(serializedOffer: string): Promise<string> {
    const { fromPeerId, sdp } = JSON.parse(serializedOffer) as { fromPeerId: PeerId; sdp: RTCSessionDescriptionInit };
    const link = new PeerLink(fromPeerId, this.transportHandlers(), false);
    this.links.set(fromPeerId, link);
    const answer = await link.createAnswer(sdp);
    return JSON.stringify({ fromPeerId: this.selfId, sdp: answer });
  }

  /** Cote initiateur : finalise le bootstrap avec la reponse recue hors-bande. */
  async completeBootstrap(serializedAnswer: string): Promise<void> {
    const { fromPeerId, sdp } = JSON.parse(serializedAnswer) as { fromPeerId: PeerId; sdp: RTCSessionDescriptionInit };
    const link = this.links.get(fromPeerId);
    if (link) {
      await link.acceptAnswer(sdp);
    }
  }

  // --- Croissance serverless du maillage : relais via les pairs deja connectes ---

  private handleControlMessage(fromPeerId: PeerId, message: ControlMessage): void {
    this.routingTable.upsert(fromPeerId);

    switch (message.type) {
      case "signal-relay":
        void this.handleSignalRelay(message);
        return;
      case "dht-find-node":
        this.handleFindNode(fromPeerId, message);
        return;
      case "dht-find-node-response": {
        const resolve = this.pendingFindNode.get(message.requestId);
        if (resolve) {
          this.pendingFindNode.delete(message.requestId);
          resolve(message.peers);
        }
        return;
      }
    }
  }

  private async handleSignalRelay(envelope: SignalEnvelope): Promise<void> {
    if (envelope.toPeerId === this.selfId) {
      await this.applyIncomingSignal(envelope);
      return;
    }

    if (envelope.ttl <= 0) {
      return; // duree de vie epuisee : abandon du relais (evite les boucles infinies)
    }

    const nextHop = this.routingTable.closestTo(envelope.toPeerId, 1)[0];
    const link = nextHop ? this.links.get(nextHop) : undefined;
    if (link) {
      link.sendControl({ ...envelope, ttl: envelope.ttl - 1 });
    }
  }

  private async applyIncomingSignal(envelope: SignalEnvelope): Promise<void> {
    const { fromPeerId, payload } = envelope;

    if (payload.kind === "offer") {
      const link = new PeerLink(fromPeerId, this.transportHandlers(), false);
      this.links.set(fromPeerId, link);
      const answer = await link.createAnswer(payload.sdp);
      this.relaySignal(fromPeerId, { kind: "answer", sdp: answer });
      return;
    }

    const link = this.links.get(fromPeerId);
    if (!link) {
      return;
    }

    if (payload.kind === "answer") {
      await link.acceptAnswer(payload.sdp);
    } else if (payload.kind === "ice-candidate") {
      await link.addIceCandidate(payload.candidate);
    }
  }

  /** Etablit une nouvelle connexion vers `targetId` en relayant l'offre via le maillage existant (aucun serveur). */
  async connectViaMeshRelay(targetId: PeerId): Promise<void> {
    const link = new PeerLink(targetId, this.transportHandlers(), true);
    this.links.set(targetId, link);
    const offer = await link.createOffer();
    this.relaySignal(targetId, { kind: "offer", sdp: offer });
  }

  private relaySignal(toPeerId: PeerId, payload: SignalPayload): void {
    const envelope: SignalEnvelope = {
      type: "signal-relay",
      fromPeerId: this.selfId,
      toPeerId,
      ttl: SIGNAL_RELAY_MAX_TTL,
      payload,
    };

    const directLink = this.links.get(toPeerId);
    if (directLink) {
      directLink.sendControl(envelope);
      return;
    }

    const nextHop = this.routingTable.closestTo(toPeerId, 1)[0];
    const relayLink = nextHop ? this.links.get(nextHop) : undefined;
    relayLink?.sendControl(envelope);
  }

  // --- Decouverte Kademlia (FIND_NODE) ---

  private handleFindNode(fromPeerId: PeerId, request: DhtFindNodeRequest): void {
    const closest = this.routingTable.closestTo(request.targetId, K_BUCKET_SIZE);
    const link = this.links.get(fromPeerId);
    link?.sendControl({ type: "dht-find-node-response", requestId: request.requestId, peers: closest });
  }

  /** Lookup Kademlia iteratif : converge vers les pairs les plus proches de `targetId` en O(log N) sauts. */
  async findNode(targetId: PeerId, timeoutMs = 3000): Promise<PeerId[]> {
    let frontier = this.routingTable.closestTo(targetId, K_BUCKET_SIZE);
    const visited = new Set<PeerId>([this.selfId]);

    for (let hop = 0; hop < ID_BITS; hop += 1) {
      const unvisited = frontier.filter((p) => !visited.has(p));
      if (unvisited.length === 0) {
        break;
      }

      const responses = await Promise.all(unvisited.map((peerId) => this.queryFindNode(peerId, targetId, timeoutMs)));
      unvisited.forEach((p) => visited.add(p));

      const discovered = responses.flat();
      const nextFrontier = [...frontier, ...discovered]
        .filter((p, idx, arr) => arr.indexOf(p) === idx)
        .sort((a, b) => {
          const da = xorDistance(a, targetId);
          const db = xorDistance(b, targetId);
          return da < db ? -1 : da > db ? 1 : 0;
        })
        .slice(0, K_BUCKET_SIZE);

      if (nextFrontier.every((p) => frontier.includes(p))) {
        break; // convergence : aucune amelioration au dernier saut
      }
      frontier = nextFrontier;
    }

    return frontier;
  }

  private queryFindNode(peerId: PeerId, targetId: PeerId, timeoutMs: number): Promise<PeerId[]> {
    const link = this.links.get(peerId);
    if (!link) {
      return Promise.resolve([]);
    }

    const requestId = generatePeerId();
    return new Promise<PeerId[]>((resolve) => {
      const timer = setTimeout(() => {
        this.pendingFindNode.delete(requestId);
        resolve([]);
      }, timeoutMs);

      this.pendingFindNode.set(requestId, (peers) => {
        clearTimeout(timer);
        resolve([...peers]);
      });

      link.sendControl({ type: "dht-find-node", requestId, targetId });
    });
  }

  broadcastSync(data: ArrayBuffer): void {
    for (const link of this.links.values()) {
      link.sendSync(data);
    }
  }

  sendSyncTo(peerId: PeerId, data: ArrayBuffer): void {
    this.links.get(peerId)?.sendSync(data);
  }

  disconnectAll(): void {
    for (const link of this.links.values()) {
      link.close();
    }
    this.links.clear();
  }
}
