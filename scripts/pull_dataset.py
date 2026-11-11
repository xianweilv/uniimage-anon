#!/usr/bin/env python3
layer blob（tar.gz）experiment

:
  python3 scripts/pull_dataset.py [--out data/images] [--classes base,ai,app] [--jobs 4]

output:
  data/images/<class>/<repo>__<tag>/
      index.json        # imagelayer/size/mediaType）
      layers/NN.tar.gz  # layer blob（ gzip layer）
"""
import argparse
import concurrent.futures as cf
import json
import os
import subprocess
import time
import sys
import tarfile
import tempfile

CRANE = os.path.expanduser("~/bin/crane")
MIRROR = os.environ.get("IMG_MIRROR", "docker.m.daocloud.io")
MIRRORS = ["docker.m.daocloud.io", "public.ecr.aws/docker"]

DATASET = {
    "base": [
        ("library/alpine", [
                          "3.18.0", "3.19.0", "3.19.1", "3.20.0", "3.20.1",
                          "3.21.0", "3.21.1", "3.21.2", "3.21.3"
        ]),
        ("library/ubuntu", [
                          "22.04", "23.10", "24.04", "24.10", "25.04"
        ]),
        ("library/debian", [
                          "bullseye", "bullseye-slim", "bookworm",
                          "bookworm-slim", "trixie", "trixie-slim"
        ]),
        ("library/busybox", [
                          "1.35.0", "1.36.0", "1.36.1", "1.37.0"
        ]),
        ("library/python", [
                          "3.9-slim", "3.10-slim-bookworm",
                          "3.11-slim-bookworm", "3.12-slim-bookworm",
                          "3.13-bookworm", "3.13-slim-bookworm",
                          "3.11-bookworm", "3.12-bookworm"
        ]),
        ("library/node", [
                          "18-slim", "20-slim", "22-slim", "24-slim",
                          "18-bookworm", "20-bookworm", "22-bookworm",
                          "24-bookworm"
        ]),
        ("library/golang", [
                          "1.19-bookworm", "1.20-bookworm", "1.21-bookworm",
                          "1.22-bookworm", "1.23-bookworm",
                          "1.22.1-bookworm", "1.22.3-bookworm",
                          "1.23.1-bookworm", "1.23.3-bookworm",
                          "1.24.0-bookworm", "1.24-bookworm",
                          "1.21.3-bookworm", "1.21.5-bookworm",
                          "1.22.2-bookworm", "1.23.4-bookworm",
                          "1.22.0-bookworm", "1.23.0-bookworm",
                          "1.24.1-bookworm"
        ]),
        ("library/rockylinux", [
                          "8.9", "9.0", "9.1", "9.2", "9.3"
        ]),
        ("library/gcc", [
                          "11-bullseye", "12-bookworm", "13-bookworm",
                          "14-bookworm", "11.3.0-bullseye",
                          "11.4.0-bullseye", "12.3.0-bookworm",
                          "12.4.0-bookworm", "15-bookworm",
                          "11.2.0-bullseye", "12.1.0-bullseye",
                          "13.2.0-bookworm", "13.3.0-bookworm",
                          "14.1.0-bookworm", "14.2.0-bookworm",
                          "10.5.0-bullseye", "11.1.0-bullseye"
        ]),
        ("library/buildpack-deps", [
                          "bullseye", "bullseye-curl", "bookworm",
                          "bookworm-curl", "trixie", "trixie-curl",
                          "bookworm-scm", "bullseye-scm"
        ]),
        ("library/julia", [
                          "1.10-bookworm", "1.11-bookworm", "1.9-bookworm"
        ]),
        ("library/ibm-semeru-runtimes", [
                          "open-17-jdk-jammy", "open-21-jdk-jammy"
        ]),
        ("library/rust", [
                          "1.70-bookworm", "1.72-bookworm", "1.74-bookworm",
                          "1.76-bookworm", "1.78-bookworm", "1.80-bookworm",
                          "1.82-bookworm", "1.75-bookworm", "1.77-bookworm",
                          "1.79-bookworm", "1.81-bookworm", "1.83-bookworm",
                          "1.84-bookworm", "1.85-bookworm", "1.73-bookworm",
                          "1.86-bookworm", "1.87-bookworm"
        ]),
        ("library/openjdk", [
                          "21-slim-bookworm", "24-slim-bookworm"
        ]),
        ("library/eclipse-temurin", [
                          "8-jdk-jammy", "11-jdk-jammy", "17-jdk-jammy",
                          "17-jdk-focal", "21-jdk-jammy", "11-jdk-focal"
        ]),
        ("library/php", [
                          "8.1-cli-bookworm", "8.2-cli-bookworm",
                          "8.3-cli-bookworm"
        ]),
        ("library/ruby", [
                          "3.0-slim", "3.1-slim", "3.2-bookworm",
                          "3.3-bookworm"
        ]),
        ("library/perl", [
                          "5.34-slim", "5.36-slim", "5.38-bookworm",
                          "5.40-bookworm"
        ]),
        ("library/haskell", [
                          "9.2-slim-buster", "9.4-slim-buster"
        ]),
        ("library/erlang", [
                          "24-slim", "25-slim", "26-slim", "27-slim"
        ]),
        ("mcr.microsoft.com/dotnet/sdk", [
                          "6.0-bullseye-slim", "7.0-bookworm-slim",
                          "8.0-bookworm-slim", "9.0-bookworm-slim"
        ]),
        ("library/gradle", [
                          "7-jdk17", "8-jdk17", "8-jdk21"
        ]),
        ("library/maven", [
                          "3.8-eclipse-temurin-11", "3.9-eclipse-temurin-17",
                          "3.9-eclipse-temurin-21"
        ]),
        ("library/scala", [

        ]),
        ("library/clojure", [

        ]),
        ("library/amazoncorretto", [
                          "17", "21"
        ]),
        ("mcr.microsoft.com/dotnet/aspnet", [
                          "6.0-bullseye-slim", "8.0-bookworm-slim"
        ]),
        ("mcr.microsoft.com/dotnet/runtime", [
                          "8.0-bookworm-slim"
        ]),
    ],
    "ai": [
        ("pytorch/pytorch", [
                          "2.1.0-cuda11.8-cudnn8-runtime",
                          "2.3.1-cuda12.1-cudnn8-runtime",
                          "2.5.1-cuda12.4-cudnn9-runtime",
                          "2.7.1-cuda12.6-cudnn9-runtime",
                          "2.9.0-cuda12.8-cudnn9-runtime"
        ]),
        ("nvidia/cuda", [
                          "11.8.0-runtime-ubuntu22.04",
                          "12.0.0-runtime-ubuntu22.04",
                          "12.2.0-runtime-ubuntu22.04",
                          "12.4.0-runtime-ubuntu22.04",
                          "12.6.0-runtime-ubuntu22.04",
                          "12.8.0-runtime-ubuntu22.04",
                          "13.0.0-runtime-ubuntu24.04"
        ]),
        ("tensorflow/tensorflow", [
                          "2.13.0-gpu", "2.15.0-gpu", "2.17.0-gpu",
                          "2.19.0-gpu"
        ]),
    ],
    "app": [
        ("library/nginx", [
                          "1.25.0", "1.25.1", "1.25.2", "1.25.3", "1.25.4",
                          "1.25.5", "1.26.0", "1.26.1", "1.26.2", "1.26.3",
                          "1.27.0"
        ]),
        ("library/redis", [
                          "7.2.0", "7.2.1", "7.2.2", "7.2.3", "7.2.4",
                          "7.2.5", "7.4.0", "7.4.1", "7.4.2", "8.8.0",
                          "8.8.1", "8.8.2", "8.8.3"
        ]),
        ("library/postgres", [
                          "15.0", "15.1", "15.2", "15.3", "15.4", "15.5",
                          "15.6", "15.7", "15.8", "16.0", "16.1", "16.2",
                          "16.3", "16.4", "17.0", "17.1", "17.2", "17.5",
                          "17.3", "17.4", "18.0", "18.4"
        ]),
        ("library/mysql", [
                          "8.0.30", "8.0.31", "8.0.32", "8.0.33", "8.0.34",
                          "8.0.35", "8.0.36", "8.0.37", "8.0.38", "8.0.39",
                          "8.0.40", "8.4.0", "8.4.1", "8.4.2", "8.4.3",
                          "9.0.0", "9.1.0", "9.2.0"
        ]),
        ("library/memcached", [
                          "1.6.20", "1.6.21", "1.6.22", "1.6.23", "1.6.24",
                          "1.6.25", "1.6.26", "1.6.27", "1.6.28"
        ]),
        ("library/traefik", [
                          "v2.10.0", "v2.10.1", "v2.11.0", "v2.11.1",
                          "v3.0.0", "v3.1.0", "v3.2.0", "v3.3.0", "v3.4.0"
        ]),
        ("gitlab/gitlab-ce", [
                          "16.9.0-ce.0", "16.10.0-ce.0", "17.0.0-ce.0",
                          "17.1.0-ce.0", "17.2.0-ce.0", "17.3.0-ce.0",
                          "16.11.0-ce.0", "17.4.0-ce.0", "17.5.0-ce.0",
                          "17.6.0-ce.0"
        ]),
        ("library/elasticsearch", [
                          "8.12.0", "8.13.0", "8.14.0", "8.17.0", "8.15.0",
                          "8.16.0", "8.18.0", "8.19.0"
        ]),
        ("library/sonarqube", [
                          "9.9-community", "10.0-community",
                          "10.3-community", "10.4-community",
                          "10.5-community", "10.7-community"
        ]),
        ("apache/spark", [
                          "3.4.1", "3.4.2", "3.5.0", "3.5.1", "3.5.2"
        ]),
        ("apache/kafka", [
                          "3.7.0"
        ]),
        ("library/mongo", [
                          "5.0", "6.0", "7.0", "7.0.14", "8.0", "7.0.8",
                          "7.0.12", "8.0.3"
        ]),
        ("neo4j", [
                          "5.10", "5.25-community", "5.15", "5.20-community"
        ]),
        ("consul", [

        ]),
        ("vault", [

        ]),
        ("library/cassandra", [
                          "4.0", "4.1"
        ]),
        ("library/jetty", [
                          "11-jdk17", "12-jdk17", "11.0-jdk17"
        ]),
        ("library/tomcat", [
                          "10.1-jdk17", "11.0-jdk17", "10.1.30-jdk17"
        ]),
        ("redmine", [
                          "5.1", "6.0"
        ]),
        ("library/joomla", [
                          "5.0", "5.1"
        ]),
        ("grafana/grafana", [
                          "9.5.0", "10.0.0", "10.4.0", "11.0.0", "11.1.0",
                          "11.3.0", "11.5.0"
        ]),
        ("prom/prometheus", [
                          "v2.48.0", "v2.50.0", "v2.51.0", "v2.53.0",
                          "v3.1.0", "v3.2.0"
        ]),
        ("library/influxdb", [
                          "2.7"
        ]),
        ("library/timescaledb", [

        ]),
        ("rabbitmq", [
                          "3.12-management", "3.13-management",
                          "3.13.7-management", "4.0-management",
                          "3.12.13-management"
        ]),
        ("library/wordpress", [
                          "6.4-php8.2", "6.5-php8.2", "6.6-php8.2",
                          "6.7-php8.2"
        ]),
        ("library/kibana", [
                          "8.12.0", "8.14.0", "8.15.0", "8.16.0", "8.17.0",
                          "8.18.0", "8.19.0", "8.11.0"
        ]),
        ("library/logstash", [
                          "8.12.0", "8.13.0", "8.15.0", "8.16.0", "8.17.0"
        ]),
    ],
}


def pull_one(cls, repo, tag, out_root):
    name = f"{repo.split('/')[-1]}__{tag}".replace(":", "-")
    dst = os.path.join(out_root, cls, name)
    idx_path = os.path.join(dst, "index.json")
    if os.path.exists(idx_path):
        return (cls, repo, tag, "exists", dst)
    os.makedirs(os.path.join(dst, "layers"), exist_ok=True)
    first = repo.split("/")[0]
    direct = "." in first              #  ->
    candidates = [repo] if direct else [f"{m}/{repo}" for m in MIRRORS]

    with tempfile.TemporaryDirectory() as td:
        oci_tar = os.path.join(td, "image.tar")
        ok = False
        last_err = ""
        for attempt in range(12):
            ref = f"{candidates[attempt % len(candidates)]}:{tag}"
            try:
                r = subprocess.run(
                    [CRANE, "pull", "--platform", "linux/amd64", ref, oci_tar],
                    capture_output=True, text=True, timeout=2400)
                if r.returncode == 0:
                    ok = True
                    break
                last_err = r.stderr[:120]
            except subprocess.TimeoutExpired:
                last_err = "timeout"
                try:
                    os.remove(oci_tar)
                except OSError:
                    pass
            time.sleep(20)   # image
        if not ok:
            return (cls, repo, tag, f"pull-fail: {last_err}", dst)

        try:
            tf = tarfile.open(oci_tar)
            members = tf.getnames()
            man = None
            for m in members:
                if m.endswith("manifest.json"):
                    man = json.load(tf.extractfile(m))
                    break
            if man is None:
                return (cls, repo, tag, "no-manifest", dst)
            config = man[0].get("Config", "")
            layers = man[0].get("Layers", [])

            info = {
                "repo": repo, "tag": tag, "class": cls, "ref": ref,
                "config": config, "layers": [],
            }
            # config
            if config in members:
                tf.extract(config, td)
                info["config_sha"] = config.split("sha256:")[-1]

            for i, lname in enumerate(layers):
                digest = lname.split("sha256:")[-1]
                tf.extract(lname, td)
                src = os.path.join(td, lname)
                with open(src, "rb") as f:
                    magic = f.read(2)
                is_gz = magic == b"\x1f\x8b"
                entry = {"digest": "sha256:" + digest, "gz": is_gz}
                sz = os.path.getsize(src)
                entry["size"] = sz
                if is_gz:
                    out_layer = os.path.join(dst, "layers", f"{i:02d}.tar.gz")
                    os.replace(src, out_layer)
                else:
                    out_layer = os.path.join(dst, "layers", f"{i:02d}.blob")
                    os.replace(src, out_layer)
                entry["file"] = os.path.relpath(out_layer, dst)
                info["layers"].append(entry)
            with open(idx_path, "w") as f:
                json.dump(info, f, indent=1)
        except Exception as e:
            return (cls, repo, tag, f"unpack-fail: {e}", dst)

    return (cls, repo, tag, "ok", dst)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "data", "images"))
    ap.add_argument("--classes", default="base,ai,app")
    ap.add_argument("--jobs", type=int, default=2)
    args = ap.parse_args()

    out_root = os.path.abspath(args.out)
    os.makedirs(out_root, exist_ok=True)
    tasks = []
    for cls in args.classes.split(","):
        for repo, tags in DATASET.get(cls, []):
            for tag in tags:
                tasks.append((cls, repo, tag))

    print(f" {len(tasks)} image -> {out_root}", flush=True)
    ok = fail = skip = 0
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(pull_one, c, r, t, out_root): (c, r, t) for c, r, t in tasks}
        for fut in cf.as_completed(futs):
            c, r, t = futs[fut]
            try:
                cls, repo, tag, status, dst = fut.result()
            except Exception as e:
                status, dst = f"exc: {e}", ""
            if status == "ok":
                ok += 1
                print(f"[ok] {cls}/{repo}:{tag}", flush=True)
            elif status == "exists":
                skip += 1
            else:
                fail += 1
                print(f"[FAIL] {cls}/{repo}:{tag} {status}", flush=True)
    print(f": ok={ok} skip={skip} fail={fail}")


if __name__ == "__main__":
    main()
