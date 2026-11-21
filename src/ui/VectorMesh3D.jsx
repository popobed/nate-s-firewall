import { useEffect, useRef } from "react";
import * as THREE from "three";
import { peerIdToSpherePosition, projectTo3D } from "./projection";

/**
 * Nebuleuse 3D en mode sombre : les points forment le graphe vectoriel
 * HNSW (projete en 3D par PCA si la dimension d'origine depasse 3), et
 * les spheres reliees par des aretes representent le maillage P2P
 * (peerIds connus, activeConnections = paires [peerIdA, peerIdB]
 * actuellement etablies).
 */
export default function VectorMesh3D({
  vectors = [],
  dim = 3,
  peerIds = [],
  activeConnections = [],
  width = 800,
  height = 600,
}) {
  const containerRef = useRef(null);

  useEffect(() => {
    const container = containerRef.current;
    if (!container) {
      return undefined;
    }

    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x05070d);
    scene.fog = new THREE.FogExp2(0x05070d, 0.02);

    const camera = new THREE.PerspectiveCamera(60, width / height, 0.1, 1000);
    camera.position.set(0, 4, 32);
    camera.lookAt(0, 0, 0);

    const renderer = new THREE.WebGLRenderer({ antialias: true });
    renderer.setSize(width, height);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    container.appendChild(renderer.domElement);

    // --- Nebuleuse HNSW ---
    const projected = projectTo3D(vectors, dim);
    const positionsArray = new Float32Array(projected.length * 3);
    projected.forEach((p, i) => {
      positionsArray[i * 3 + 0] = p[0] ?? 0;
      positionsArray[i * 3 + 1] = p[1] ?? 0;
      positionsArray[i * 3 + 2] = p[2] ?? 0;
    });

    const pointsGeometry = new THREE.BufferGeometry();
    pointsGeometry.setAttribute("position", new THREE.BufferAttribute(positionsArray, 3));

    const pointsMaterial = new THREE.PointsMaterial({
      color: 0x7dd3fc,
      size: 0.35,
      sizeAttenuation: true,
      transparent: true,
      opacity: 0.85,
    });
    const pointCloud = new THREE.Points(pointsGeometry, pointsMaterial);
    scene.add(pointCloud);

    // --- Maillage P2P ---
    const peerGroup = new THREE.Group();
    const peerPositions = new Map();

    peerIds.forEach((peerId) => {
      const [x, y, z] = peerIdToSpherePosition(peerId);
      peerPositions.set(peerId, new THREE.Vector3(x, y, z));

      const nodeMesh = new THREE.Mesh(
        new THREE.SphereGeometry(0.4, 12, 12),
        new THREE.MeshBasicMaterial({ color: 0xf472b6 }),
      );
      nodeMesh.position.set(x, y, z);
      peerGroup.add(nodeMesh);
    });

    activeConnections.forEach(([peerA, peerB]) => {
      const positionA = peerPositions.get(peerA);
      const positionB = peerPositions.get(peerB);
      if (!positionA || !positionB) {
        return;
      }
      const lineGeometry = new THREE.BufferGeometry().setFromPoints([positionA, positionB]);
      const lineMaterial = new THREE.LineBasicMaterial({ color: 0x38bdf8, transparent: true, opacity: 0.5 });
      peerGroup.add(new THREE.Line(lineGeometry, lineMaterial));
    });

    scene.add(peerGroup);
    scene.add(new THREE.AmbientLight(0xffffff, 0.7));

    let animationFrameId = 0;
    const animate = () => {
      pointCloud.rotation.y += 0.0015;
      peerGroup.rotation.y += 0.0015;
      renderer.render(scene, camera);
      animationFrameId = requestAnimationFrame(animate);
    };
    animate();

    return () => {
      cancelAnimationFrame(animationFrameId);
      pointsGeometry.dispose();
      pointsMaterial.dispose();
      peerGroup.children.forEach((child) => {
        child.geometry?.dispose();
        child.material?.dispose();
      });
      renderer.dispose();
      if (container.contains(renderer.domElement)) {
        container.removeChild(renderer.domElement);
      }
    };
  }, [vectors, dim, peerIds, activeConnections, width, height]);

  return (
    <div
      ref={containerRef}
      style={{
        width,
        height,
        background: "#05070d",
        borderRadius: 12,
        overflow: "hidden",
        boxShadow: "0 0 40px rgba(56, 189, 248, 0.15)",
      }}
    />
  );
}
