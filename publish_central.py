#!/usr/bin/env python3
"""
Publishes an Android AAR package to Maven Central via Central Portal API.
Handles POM generation, empty sources/javadoc jars, checksums, GPG signing,
bundle.zip packaging, and uploading via https://central.sonatype.com/api/v1/publisher/upload
"""

import os
import sys
import argparse
import hashlib
import zipfile
import subprocess
import urllib.request
import urllib.error
import mimetypes
import uuid


def generate_pom(group_id: str, artifact_id: str, version: str) -> str:
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<project xmlns="http://maven.apache.org/POM/4.0.0"
         xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
         xsi:schemaLocation="http://maven.apache.org/POM/4.0.0 http://maven.apache.org/xsd/maven-4.0.0.xsd">
    <modelVersion>4.0.0</modelVersion>
    <groupId>{group_id}</groupId>
    <artifactId>{artifact_id}</artifactId>
    <version>{version}</version>
    <packaging>aar</packaging>

    <name>{artifact_id}</name>
    <description>Custom unified NCNN Android AAR for Pixiv-Shaft (arm64-v8a with Vulkan, dynamically linked realcugan, rife, realsr, rembg)</description>
    <url>https://github.com/wangwang-code/ncnn-android-shaft</url>

    <licenses>
        <license>
            <name>BSD 3-Clause License</name>
            <url>https://opensource.org/licenses/BSD-3-Clause</url>
        </license>
    </licenses>

    <developers>
        <developer>
            <id>wangwang-code</id>
            <name>wangwang-code</name>
            <url>https://github.com/wangwang-code</url>
        </developer>
    </developers>

    <scm>
        <connection>scm:git:git://github.com/wangwang-code/ncnn-android-shaft.git</connection>
        <developerConnection>scm:git:ssh://github.com:wangwang-code/ncnn-android-shaft.git</developerConnection>
        <url>https://github.com/wangwang-code/ncnn-android-shaft</url>
    </scm>
</project>
"""


def create_stub_jar(dest_path: str, readme_content: str):
    with zipfile.ZipFile(dest_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("META-INF/MANIFEST.MF", "Manifest-Version: 1.0\nCreated-By: Pixiv-Shaft Builder\n")
        zf.writestr("README.md", readme_content)


def compute_checksums(file_path: str):
    with open(file_path, "rb") as f:
        data = f.read()
    md5 = hashlib.md5(data).hexdigest()
    sha1 = hashlib.sha1(data).hexdigest()
    with open(file_path + ".md5", "w", encoding="utf-8") as f:
        f.write(md5)
    with open(file_path + ".sha1", "w", encoding="utf-8") as f:
        f.write(sha1)


def gpg_sign_file(file_path: str, passphrase: str = None):
    cmd = ["gpg", "--batch", "--yes", "--armor", "--pinentry-mode", "loopback", "--detach-sign"]
    if passphrase:
        cmd.extend(["--passphrase", passphrase])
    cmd.append(file_path)
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"GPG signing failed for {file_path}:\n{res.stderr}", file=sys.stderr)
        sys.exit(res.returncode)


def upload_bundle_to_central(bundle_path: str, token: str, publishing_type: str = "AUTOMATIC"):
    url = f"https://central.sonatype.com/api/v1/publisher/upload?publishingType={publishing_type}"
    boundary = f"----WebKitFormBoundary{uuid.uuid4().hex}"
    
    with open(bundle_path, "rb") as f:
        file_bytes = f.read()

    body = bytearray()
    body.extend(f"--{boundary}\r\n".encode("utf-8"))
    body.extend(f'Content-Disposition: form-data; name="bundle"; filename="{os.path.basename(bundle_path)}"\r\n'.encode("utf-8"))
    body.extend(b"Content-Type: application/octet-stream\r\n\r\n")
    body.extend(file_bytes)
    body.extend(f"\r\n--{boundary}--\r\n".encode("utf-8"))

    # Format authorization token:
    token_str = token.strip()
    if token_str.startswith("Bearer "):
        token_str = token_str[7:].strip()
    
    # If the user passed raw 'username:password', encode to base64
    if ":" in token_str and not token_str.endswith("="):
        import base64
        token_str = base64.b64encode(token_str.encode("utf-8")).decode("utf-8")

    auth_header = f"Bearer {token_str}"

    req = urllib.request.Request(url, data=body, method="POST")
    req.add_header("Authorization", auth_header)
    req.add_header("Content-Type", f"multipart/form-data; boundary={boundary}")
    req.add_header("User-Agent", "Pixiv-Shaft-Publisher/1.0")

    print(f"Uploading bundle to Central Portal: {url} ...")
    try:
        with urllib.request.urlopen(req) as resp:
            resp_body = resp.read().decode("utf-8")
            print(f"Upload successful (HTTP {resp.status})!")
            print(f"Response: {resp_body}")
            print("\nTrack deployment progress at: https://central.sonatype.com/publishing/deployments")
    except urllib.error.HTTPError as e:
        error_msg = e.read().decode("utf-8", errors="ignore")
        print(f"Failed to upload to Central Portal: HTTP {e.code} {e.reason}\nResponse: {error_msg}", file=sys.stderr)
        sys.exit(1)


def main():
    parser = argparse.ArgumentParser(description="Package and publish AAR to Maven Central Portal")
    parser.add_argument("--aar", required=True, help="Path to input .aar file")
    parser.add_argument("--group-id", default="io.github.wangwang-code", help="Maven Group ID")
    parser.add_argument("--artifact-id", default="ncnn-android-shaft", help="Maven Artifact ID")
    parser.add_argument("--version", default="20241226-shaft-1", help="Artifact version")
    parser.add_argument("--out-dir", default="./central_bundle", help="Output staging directory")
    parser.add_argument("--gpg-passphrase", default="", help="GPG private key passphrase")
    parser.add_argument("--central-token", default="", help="Sonatype Central Portal User Token")
    parser.add_argument("--publishing-type", default="AUTOMATIC", choices=["AUTOMATIC", "USER_MANAGED"])
    parser.add_argument("--skip-upload", action="store_true", help="Only build bundle.zip, do not upload")

    args = parser.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    prefix = f"{args.artifact_id}-{args.version}"

    aar_dest = os.path.join(args.out_dir, f"{prefix}.aar")
    pom_dest = os.path.join(args.out_dir, f"{prefix}.pom")
    sources_dest = os.path.join(args.out_dir, f"{prefix}-sources.jar")
    javadoc_dest = os.path.join(args.out_dir, f"{prefix}-javadoc.jar")

    # 1. Copy AAR
    with open(args.aar, "rb") as sf, open(aar_dest, "wb") as df:
        df.write(sf.read())

    # 2. Write POM
    with open(pom_dest, "w", encoding="utf-8") as f:
        f.write(generate_pom(args.group_id, args.artifact_id, args.version))

    # 3. Create Sources & Javadoc jars
    create_stub_jar(sources_dest, f"Sources for {args.artifact_id}:{args.version}\nC++ native prebuilt library.\n")
    create_stub_jar(javadoc_dest, f"Javadoc for {args.artifact_id}:{args.version}\n")

    files_to_process = [aar_dest, pom_dest, sources_dest, javadoc_dest]

    # 4. GPG sign and generate checksums
    print("Generating GPG signatures (.asc) and checksums (.md5, .sha1)...")
    for f in files_to_process:
        compute_checksums(f)
        gpg_sign_file(f, args.gpg_passphrase if args.gpg_passphrase else None)

    # 5. Build bundle.zip with standard Maven coordinate path structure
    bundle_zip = os.path.join(os.path.dirname(args.out_dir) or ".", "bundle.zip")
    group_rel_path = args.group_id.replace(".", "/")
    target_rel_dir = f"{group_rel_path}/{args.artifact_id}/{args.version}"
    print(f"Creating bundle: {bundle_zip} with directory path: {target_rel_dir} ...")
    with zipfile.ZipFile(bundle_zip, "w", zipfile.ZIP_DEFLATED) as zf:
        for root, _, filenames in os.walk(args.out_dir):
            for filename in filenames:
                file_full = os.path.join(root, filename)
                arc_name = f"{target_rel_dir}/{filename}"
                zf.write(file_full, arcname=arc_name)

    print(f"Bundle successfully created: {bundle_zip} (size: {os.path.getsize(bundle_zip)} bytes)")

    # 6. Upload if token provided and not skipped
    if args.central_token and not args.skip_upload:
        upload_bundle_to_central(bundle_zip, args.central_token, args.publishing_type)
    else:
        print("Upload skipped (either --skip-upload specified or --central-token not provided).")


if __name__ == "__main__":
    main()
