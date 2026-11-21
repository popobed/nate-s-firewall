import { describe, expect, it } from "vitest";
import { VectorOrSet, buildMerkleDag, resyncViaMerkleDag, type MerkleNode, type VectorRecord } from "./crdt_sync";

function record(vectorId: string, values: number[]): VectorRecord {
  return { vectorId, dim: values.length, values };
}

describe("VectorOrSet", () => {
  it("merges two replicas that diverged offline into an identical state", () => {
    const alice = new VectorOrSet("alice");
    const bob = new VectorOrSet("bob");

    alice.add(record("v1", [1, 2, 3]));
    bob.add(record("v2", [4, 5, 6]));

    // Fusion croisee, dans les deux sens, plusieurs fois (idempotence).
    alice.merge(bob.toSnapshot());
    bob.merge(alice.toSnapshot());
    alice.merge(bob.toSnapshot());

    expect(alice.size()).toBe(2);
    expect(bob.size()).toBe(2);
    expect(alice.has("v1")).toBe(true);
    expect(alice.has("v2")).toBe(true);
    expect(bob.has("v1")).toBe(true);
    expect(bob.has("v2")).toBe(true);
  });

  it("is commutative and associative regardless of merge order", () => {
    const a = new VectorOrSet("a");
    const b = new VectorOrSet("b");
    const c = new VectorOrSet("c");
    a.add(record("x", [1]));
    b.add(record("y", [2]));
    c.add(record("z", [3]));

    const order1 = new VectorOrSet("observer1");
    order1.merge(a.toSnapshot());
    order1.merge(b.toSnapshot());
    order1.merge(c.toSnapshot());

    const order2 = new VectorOrSet("observer2");
    order2.merge(c.toSnapshot());
    order2.merge(a.toSnapshot());
    order2.merge(b.toSnapshot());

    expect(order1.values().map((v) => v.vectorId).sort()).toEqual(
      order2.values().map((v) => v.vectorId).sort(),
    );
  });

  it("add-wins: a concurrent add survives a remove that did not observe it", () => {
    const alice = new VectorOrSet("alice");
    const bob = new VectorOrSet("bob");

    alice.add(record("shared", [1, 1]));
    bob.merge(alice.toSnapshot());

    // Bob supprime sa copie SANS savoir qu'Alice va re-ajouter concurremment.
    bob.remove("shared");
    alice.add(record("shared", [9, 9])); // second tag d'ajout, concurrent a la suppression de Bob

    bob.merge(alice.toSnapshot());
    alice.merge(bob.toSnapshot());

    expect(bob.has("shared")).toBe(true); // add-wins : l'ajout concurrent survit
    expect(alice.has("shared")).toBe(true);
  });

  it("a remove observed by both sides removes the element for both", () => {
    const alice = new VectorOrSet("alice");
    alice.add(record("v1", [1]));

    const bob = new VectorOrSet("bob");
    bob.merge(alice.toSnapshot());

    alice.remove("v1");
    bob.merge(alice.toSnapshot());

    expect(bob.has("v1")).toBe(false);
  });
});

describe("MerkleDag resync", () => {
  it("produces identical roots for identical content regardless of insertion order", async () => {
    const recordsA = [record("v1", [1]), record("v2", [2]), record("v3", [3])];
    const recordsB = [record("v3", [3]), record("v1", [1]), record("v2", [2])];

    const rootA = await buildMerkleDag(recordsA);
    const rootB = await buildMerkleDag(recordsB);

    expect(rootA.hash).toBe(rootB.hash);
  });

  it("detects a single differing vector without transferring the full dataset", async () => {
    const shared = Array.from({ length: 50 }, (_, i) => record(`v${i}`, [i]));
    const localRecords = [...shared];
    const remoteRecords = shared.map((r) => (r.vectorId === "v37" ? record("v37", [999]) : r));

    const localRoot = await buildMerkleDag(localRecords);
    const remoteRoot = await buildMerkleDag(remoteRecords);

    // Les deux ensembles partagent le meme jeu d'identifiants, dans le
    // meme ordre trie : les deux arbres ont donc EXACTEMENT la meme
    // forme, seuls certains hachages differents le long du chemin vers
    // "v37". On associe une fois chaque noeud local a son homologue
    // distant par simple parcours parallele, ce qui simule fidelement ce
    // qu'un vrai transport reseau renverrait a une requete "enfants de
    // ce noeud", sans jamais transferer l'ensemble des donnees.
    const correspondence = new Map<MerkleNode, MerkleNode>();
    zipCorrespondingNodes(localRoot, remoteRoot, correspondence);

    let roundTrips = 0;
    const fetchRemoteChildHashes = async (localNode: MerkleNode): Promise<readonly string[]> => {
      roundTrips += 1;
      const remoteNode = correspondence.get(localNode);
      return remoteNode ? remoteNode.children.map((c) => c.hash) : [];
    };

    const divergent = await resyncViaMerkleDag(localRoot, remoteRoot.hash, fetchRemoteChildHashes);

    expect(divergent).toEqual(["v37"]);
    // O(log N) : le nombre d'allers-retours doit rester bien inferieur a N=50.
    expect(roundTrips).toBeLessThan(10);
  });
});

function zipCorrespondingNodes(local: MerkleNode, remote: MerkleNode, map: Map<MerkleNode, MerkleNode>): void {
  map.set(local, remote);
  for (let i = 0; i < local.children.length; i += 1) {
    const localChild = local.children[i];
    const remoteChild = remote.children[i];
    if (localChild && remoteChild) {
      zipCorrespondingNodes(localChild, remoteChild, map);
    }
  }
}
