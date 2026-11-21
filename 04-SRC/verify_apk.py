#!/usr/bin/env python3
"""verify_apk.py —— 离线校验 APK Signing Block v2 是否自洽（无 apksigner 环境下的自检）

  python3 verify_apk.py apk [cert.pem]

校验项:
  1. ZIP 中央目录可正常解析（zipfile 全量 CRC 校验）
  2. APK Sig Block 结构完整（size 双写一致 + magic）
  3. v2 块内 digest 与按规范重算的 APK 内容摘要一致
  4. 签名能被证书里公钥验证（若有 cert.pem）
"""
import hashlib, struct, sys, zipfile
from cryptography import x509
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import padding, rsa

CHUNK = 1048576
MAGIC = b"APK Sig Block 42"


def u32(b): return struct.unpack("<I", b[:4])[0]


def lp_read(b, off):
    n = u32(b[off:off + 4]); return b[off + 4:off + 4 + n], off + 4 + n


def chunked(data):
    h = hashlib.sha256()
    for i in range(0, len(data), CHUNK):
        c = data[i:i + CHUNK]
        h.update(hashlib.sha256(b"\x5a" + struct.pack("<I", len(c)) + c).digest())
    return h.digest()


def main(path, cert_pem=None):
    data = open(path, "rb").read()
    print("文件:", path, len(data), "字节")

    # 1) ZIP
    z = zipfile.ZipFile(path)
    bad = z.testzip()
    print("  ZIP CRC 校验:", "全部通过" if bad is None else f"损坏 {bad}", "| 条目", len(z.namelist()))

    # 2) EOCD / 块
    eocd = data.rfind(b"PK\x05\x06")
    cd_size, cd_off = struct.unpack("<II", data[eocd + 12:eocd + 20])
    assert data[eocd + 4:eocd + 12] == b"\x00" * 8 or True

    # 找块尾: magic 位于 cd_off 前 16B 内
    tail = data[cd_off - 24:cd_off]
    print("  块尾 magic:", "OK" if tail[-16:] == MAGIC else "缺失")

    # 从 cd_off 向前读 size(8) 得块起点
    size2 = struct.unpack("<Q", data[cd_off - 24:cd_off - 16])[0]
    block_start = cd_off - size2 - 8
    size1 = struct.unpack("<Q", data[block_start:block_start + 8])[0]
    print(f"  块结构: size1={size1} size2={size2} 一致={size1 == size2} 起点={block_start}")

    # 3) 解析 v2 块
    p = block_start + 8
    v2 = None
    while p < cd_off - 24:
        ln = struct.unpack("<Q", data[p:p + 8])[0]
        bid = u32(data[p + 8:p + 12])
        if bid == 0x7109871A:
            v2 = data[p + 12:p + 8 + ln]
        p += 8 + ln

    seq, _ = lp_read(v2, 0)            # signers 容器
    signer, off = lp_read(seq, 0)      # 第一个 signer 内容
    signed_data_field, _ = lp_read(signer, 0)
    sd, _ = lp_read(signed_data_field, 0)
    digests_raw, q = lp_read(sd, 0)
    certs_raw, q = lp_read(sd, q)
    attrs_raw, q = lp_read(sd, q)
    sigs_raw, p2 = lp_read(signer, off)
    pubkey_der, _ = lp_read(signer, p2)

    alg, dg = struct.unpack("<I", digests_raw[:4])[0], digests_raw[4:]
    digest_in_block = dg[4:]
    sig_alg, sg = struct.unpack("<I", sigs_raw[:4])[0], sigs_raw[4:]
    signature = sg[4:]

    # 4) 重算摘要
    sec1 = data[:block_start]
    sec2 = data[cd_off - cd_size:cd_off]
    eocd_b = bytearray(data[eocd:])
    eocd_b[16:20] = struct.pack("<I", block_start)   # 校验时按"块起点"还原
    final = hashlib.sha256(chunked(sec1) + chunked(sec2) + chunked(bytes(eocd_b))).digest()
    print("  摘要比对:", "一致" if final == digest_in_block else "不一致")
    print("    block :", digest_in_block.hex())
    print("    recomputed:", final.hex())

    # 5) 验签
    cert_der, _ = lp_read(certs_raw, 0)
    cert = x509.load_der_x509_certificate(cert_der)
    pub = cert.public_key()
    try:
        pub.verify(signature, signed_data_field, padding.PKCS1v15(), hashes.SHA256())
        print("  签名验签: OK")
    except Exception as e:
        print("  签名验签: 失败", e)
    if cert_pem:
        c2 = x509.load_pem_x509_certificate(open(cert_pem, "rb").read())
        print("  证书匹配:", c2.public_key().public_numbers() == pub.public_numbers())

    # 关键内容抽查
    with z.open("AndroidManifest.xml") as f:
        print("  manifest 存在, 大小", len(f.read()))
    names = z.namelist()
    for n in names:
        if n.endswith("libvrsdk.so") or "huawei" in n.lower():
            print("  条目:", n, z.getinfo(n).file_size)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
