/**
 * CRDT vectoriel (Observed-Remove Set) et Merkle DAG de resynchronisation.
 *
 * Le CRDT garantit une fusion deterministe hors-ligne/en-ligne : deux
 * repliques qui ont divergees independamment convergent TOUJOURS vers le
 * meme etat apres `merge`, quel que soit l'ordre ou le nombre de fois ou
 * les operations sont appliquees (commutativite, associativite,
 * idempotence). Le Merkle DAG permet ensuite a deux pairs de determiner,
 * en O(log N) allers-retours, EXACTEMENT quels elements different entre
 * leurs deux repliques, sans jamais transferer l'ensemble complet.
 */

// ---------------------------------------------------------------------
// Primitives de hachage (SubtleCrypto, disponible navigateur + Node >= 15)
// ---------------------------------------------------------------------

async function sha256Hex(data: Uint8Array): Promise<string> {
  // Copie defensive dans un ArrayBuffer frais et exactement dimensionne :
  // `data` pourrait etre une sous-vue (byteOffset/length partiels) d'un
  // buffer plus grand, auquel cas `data.buffer` seul serait incorrect ;
  // et lib.dom.d.ts exige un `ArrayBuffer` concret (jamais partage) pour
  // `SubtleCrypto.digest`.
  const owned = new Uint8Array(data.byteLength);
  owned.set(data);
  const digest = await crypto.subtle.digest("SHA-256", owned.buffer);
  return Array.from(new Uint8Array(digest))
    .map((b) => b.toString(16).padStart(2, "0"))
    .join("");
}

function utf8(input: string): Uint8Array {
  return new TextEncoder().encode(input);
}

// ---------------------------------------------------------------------
// Identifiant unique d'operation : (peerId, compteur logique local au pair)
// ---------------------------------------------------------------------

export interface OperationId {
  readonly peerId: string;
  readonly counter: number;
}

function operationIdKey(id: OperationId): string {
  return `${id.peerId}:${id.counter}`;
}

export interface VectorRecord {
  readonly vectorId: string; // identifiant logique stable du vecteur (independant du CRDT)
  readonly dim: number;
  readonly values: readonly number[];
}

interface OrSetEntry {
  readonly record: VectorRecord;
  readonly addTags: Set<string>; // cles operationIdKey des additions observees
}

/**
 * Observed-Remove Set : chaque ajout est tague par un identifiant
 * d'operation unique. Une suppression retire uniquement les tags DEJA
 * OBSERVES au moment de la suppression (jamais des tags futurs encore
 * inconnus) : un ajout concurrent a une suppression "survit" toujours,
 * ce qui est la propriete recherchee ("add-wins") pour un catalogue de
 * vecteurs partage.
 */
export class VectorOrSet {
  private readonly entries = new Map<string, OrSetEntry>();
  private readonly tombstones = new Set<string>();
  private localCounter = 0;

  constructor(private readonly localPeerId: string) {}

  private nextOperationId(): OperationId {
    this.localCounter += 1;
    return { peerId: this.localPeerId, counter: this.localCounter };
  }

  add(record: VectorRecord): OperationId {
    const opId = this.nextOperationId();
    const existing = this.entries.get(record.vectorId);
    if (existing) {
      existing.addTags.add(operationIdKey(opId));
    } else {
      this.entries.set(record.vectorId, { record, addTags: new Set([operationIdKey(opId)]) });
    }
    return opId;
  }

  remove(vectorId: string): void {
    const existing = this.entries.get(vectorId);
    if (!existing) {
      return;
    }
    for (const tag of existing.addTags) {
      this.tombstones.add(tag);
    }
    this.entries.delete(vectorId);
  }

  has(vectorId: string): boolean {
    return this.entries.has(vectorId);
  }

  values(): VectorRecord[] {
    return Array.from(this.entries.values()).map((e) => e.record);
  }

  size(): number {
    return this.entries.size;
  }

  /**
   * Fusion deterministe avec une autre replique (locale ou recue d'un
   * pair distant, deserialisee via `toSnapshot`/`fromSnapshot`).
   * Commutative, associative, idempotente : `merge` peut etre appele
   * dans n'importe quel ordre, plusieurs fois, avec le meme resultat
   * final.
   */
  merge(other: VectorOrSetSnapshot): void {
    for (const tag of other.tombstones) {
      this.tombstones.add(tag);
    }

    for (const remoteEntry of other.entries) {
      const survivingTags = remoteEntry.addTags.filter((tag) => !this.tombstones.has(tag));
      if (survivingTags.length === 0) {
        continue; // toutes les additions connues de cet element ont ete supprimees
      }

      const local = this.entries.get(remoteEntry.record.vectorId);
      if (local) {
        for (const tag of survivingTags) {
          local.addTags.add(tag);
        }
      } else {
        this.entries.set(remoteEntry.record.vectorId, {
          record: remoteEntry.record,
          addTags: new Set(survivingTags),
        });
      }
    }

    // Un element local dont TOUS les tags viennent d'etre invalides par
    // des tombstones distants recus doit egalement disparaitre.
    for (const [vectorId, entry] of Array.from(this.entries.entries())) {
      const remaining = Array.from(entry.addTags).filter((tag) => !this.tombstones.has(tag));
      if (remaining.length === 0) {
        this.entries.delete(vectorId);
      }
    }
  }

  toSnapshot(): VectorOrSetSnapshot {
    return {
      tombstones: Array.from(this.tombstones),
      entries: Array.from(this.entries.values()).map((e) => ({
        record: e.record,
        addTags: Array.from(e.addTags),
      })),
    };
  }
}

export interface VectorOrSetSnapshot {
  readonly tombstones: readonly string[];
  readonly entries: ReadonlyArray<{
    readonly record: VectorRecord;
    readonly addTags: readonly string[];
  }>;
}

// ---------------------------------------------------------------------
// Merkle DAG de resynchronisation
// ---------------------------------------------------------------------

export interface MerkleNode {
  readonly hash: string;
  readonly isLeaf: boolean;
  readonly vectorId?: string; // present uniquement sur une feuille
  readonly children: readonly MerkleNode[];
}

const MERKLE_FANOUT = 4;

/**
 * Construit un Merkle DAG a partir d'un ensemble de vecteurs, tries par
 * identifiant pour garantir une structure deterministe (deux repliques
 * avec le meme contenu produisent EXACTEMENT le meme arbre, feuille par
 * feuille) independamment de l'ordre d'insertion local.
 */
export async function buildMerkleDag(records: readonly VectorRecord[]): Promise<MerkleNode> {
  const sorted = [...records].sort((a, b) => (a.vectorId < b.vectorId ? -1 : a.vectorId > b.vectorId ? 1 : 0));

  const leaves: MerkleNode[] = await Promise.all(
    sorted.map(async (record) => {
      const payload = utf8(`${record.vectorId}|${record.dim}|${record.values.join(",")}`);
      const hash = await sha256Hex(payload);
      return { hash, isLeaf: true, vectorId: record.vectorId, children: [] };
    }),
  );

  if (leaves.length === 0) {
    const emptyHash = await sha256Hex(utf8("empty"));
    return { hash: emptyHash, isLeaf: false, children: [] };
  }

  return buildLevel(leaves);
}

async function buildLevel(nodes: MerkleNode[]): Promise<MerkleNode> {
  if (nodes.length === 1) {
    return nodes[0]!;
  }

  const parents: MerkleNode[] = [];
  for (let i = 0; i < nodes.length; i += MERKLE_FANOUT) {
    const group = nodes.slice(i, i + MERKLE_FANOUT);
    const combinedHash = await sha256Hex(utf8(group.map((g) => g.hash).join("|")));
    parents.push({ hash: combinedHash, isLeaf: false, children: group });
  }

  return buildLevel(parents);
}

/**
 * Compare deux Merkle DAG (typiquement : la racine locale, et la racine
 * -- ou les enfants d'un sous-arbre -- annoncee par un pair distant) et
 * retourne les identifiants de vecteurs presents dans `local` mais dont
 * le sous-arbre differe cote distant. Utilise recursivement : a chaque
 * pair de hachages differents, on ne descend QUE dans les enfants
 * correspondants, jamais dans les branches identiques -- d'ou le cout en
 * O(log N) plutot que O(N).
 */
export function diffMerkleSubtrees(local: MerkleNode, remoteHash: string): string[] {
  if (local.hash === remoteHash) {
    return []; // sous-arbres identiques : aucune divergence, on arrete la descente ici
  }

  if (local.isLeaf) {
    return local.vectorId ? [local.vectorId] : [];
  }

  // Sans connaissance des enfants distants, le pair local doit demander
  // au distant la liste de ses hachages enfants pour ce noeud avant de
  // pouvoir descendre plus finement ; cette fonction est appelee a
  // nouveau, enfant par enfant, une fois cette reponse recue (voir
  // `MerkleSyncSession` ci-dessous pour le protocole complet).
  return local.children.flatMap((child) => diffMerkleSubtrees(child, ""));
}

/**
 * Protocole de resynchronisation complet, pilote localement : a chaque
 * etape, on ne demande au pair distant que les hachages enfants des
 * noeuds ou une divergence a ete detectee. `fetchRemoteChildHashes` est
 * fourni par la couche transport (p2p_mesh.ts) : il envoie une requete
 * sur le DataChannel et resout avec la reponse du pair.
 */
export async function resyncViaMerkleDag(
  localRoot: MerkleNode,
  remoteRootHash: string,
  fetchRemoteChildHashes: (localNode: MerkleNode) => Promise<readonly string[]>,
): Promise<string[]> {
  if (localRoot.hash === remoteRootHash) {
    return [];
  }

  if (localRoot.isLeaf) {
    return localRoot.vectorId ? [localRoot.vectorId] : [];
  }

  const remoteChildHashes = await fetchRemoteChildHashes(localRoot);
  const divergentVectorIds: string[] = [];

  for (let i = 0; i < localRoot.children.length; i += 1) {
    const child = localRoot.children[i]!;
    const remoteHash = remoteChildHashes[i];

    if (remoteHash === undefined || child.hash !== remoteHash) {
      const nested = await resyncViaMerkleDag(child, remoteHash ?? "", fetchRemoteChildHashes);
      divergentVectorIds.push(...nested);
    }
  }

  return divergentVectorIds;
}
