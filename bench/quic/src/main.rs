// Reference QUIC file transfer, used as the baseline the Fuse benchmark is
// measured against. Mirrors bench/fuse_filebench.cpp as closely as the two
// protocols allow: the client sends a file, the server receives it and
// writes it out, and both report elapsed time and throughput.
//
// Deliberate design choices for fairness:
//   * one unidirectional stream carries the whole file, so this measures
//     bulk transfer rather than QUIC's stream multiplexing,
//   * the server trusts a self-signed cert supplied out of band rather than
//     disabling certificate verification, so the TLS work is realistic,
//   * timing starts when the first byte of payload moves, excluding process
//     startup, and the connection/handshake cost is reported separately.
//
//   quic_filebench recv <port> <out-file> <cert-out>
//   quic_filebench send <host> <port> <in-file> <cert-in>

use anyhow::{anyhow, Result};
use quinn::{ClientConfig, Endpoint, ServerConfig, TransportConfig};
use rustls::pki_types::{CertificateDer, PrivatePkcs8KeyDer};
use std::net::{Ipv4Addr, SocketAddr};
use std::sync::Arc;
use std::time::Instant;

const STREAM_CHUNK: usize = 64 * 1024;

fn transport() -> TransportConfig {
    let mut t = TransportConfig::default();
    // Generous flow-control windows so the comparison measures the protocol
    // rather than a conservative default stream window.
    t.stream_receive_window((16u32 * 1024 * 1024).into());
    t.receive_window((64u32 * 1024 * 1024).into());
    t.send_window(64 * 1024 * 1024);
    t
}

// Receives one shard on its own endpoint/connection. Lanes are matched to
// ports exactly as the Fuse harness does, so the two are comparable.
async fn recv_lane(
    port: u16,
    cert_der: CertificateDer<'static>,
    key_der: PrivatePkcs8KeyDer<'static>,
) -> Result<(Vec<u8>, f64, f64)> {
    let mut server_config = ServerConfig::with_single_cert(vec![cert_der], key_der.into())?;
    server_config.transport_config(Arc::new(transport()));
    let endpoint = Endpoint::server(server_config, SocketAddr::from((Ipv4Addr::UNSPECIFIED, port)))?;

    let incoming = endpoint
        .accept()
        .await
        .ok_or_else(|| anyhow!("endpoint closed before a connection arrived"))?;
    let conn = incoming.await?;

    // Start timing when the stream opens, before any payload is read.
    // Starting after the first read would exclude data already buffered and
    // overstate throughput. Mirrors the Fuse receiver's first-datagram clock.
    let mut recv = conn.accept_uni().await?;
    let start = Instant::now();

    let mut buf = vec![0u8; STREAM_CHUNK];
    let mut shard = Vec::new();
    while let Some(n) = recv.read(&mut buf).await? {
        shard.extend_from_slice(&buf[..n]);
    }
    let elapsed = start.elapsed().as_secs_f64();
    let started_at = start.elapsed().as_secs_f64() - elapsed; // 0.0; kept for clarity

    conn.close(0u32.into(), b"done");
    endpoint.wait_idle().await;
    Ok((shard, elapsed, started_at))
}

async fn run_server(base_port: u16, lanes: u16, out_path: String, cert_path: String) -> Result<()> {
    let cert = rcgen::generate_simple_self_signed(vec!["localhost".into()])?;
    let cert_der = CertificateDer::from(cert.cert);
    let key_bytes = cert.key_pair.serialize_der();

    // Hand the certificate to the client out of band so it can verify
    // normally instead of us disabling verification.
    std::fs::write(&cert_path, cert_der.as_ref())?;

    let mut tasks = Vec::new();
    for i in 0..lanes {
        let c = cert_der.clone();
        let k = PrivatePkcs8KeyDer::from(key_bytes.clone());
        tasks.push(tokio::spawn(recv_lane(base_port + i, c, k)));
    }

    let mut shards: Vec<Vec<u8>> = Vec::with_capacity(lanes as usize);
    let mut slowest = 0.0f64;
    for t in tasks {
        let (shard, elapsed, _) = t.await??;
        slowest = slowest.max(elapsed);
        shards.push(shard);
    }

    // Stitch the shards back together in lane order.
    let mut file = Vec::new();
    for s in &shards {
        file.extend_from_slice(s);
    }
    std::fs::write(&out_path, &file)?;

    println!(
        "RECV bytes={} lanes={} elapsed={:.4} throughput={:.1} MB/s",
        file.len(),
        lanes,
        slowest,
        (file.len() as f64 / (1024.0 * 1024.0)) / slowest
    );
    Ok(())
}

async fn send_lane(
    host: String,
    port: u16,
    shard: Vec<u8>,
    client_config: ClientConfig,
) -> Result<f64> {
    let mut endpoint = Endpoint::client(SocketAddr::from((Ipv4Addr::UNSPECIFIED, 0)))?;
    endpoint.set_default_client_config(client_config);
    let addr: SocketAddr = format!("{host}:{port}").parse()?;

    let conn = endpoint.connect(addr, "localhost")?.await?;
    let start = Instant::now();
    let mut send = conn.open_uni().await?;
    send.write_all(&shard).await?;
    send.finish()?;
    // Wait for the peer to close so the measurement covers delivery, not
    // just handing bytes to the local send buffer.
    conn.closed().await;
    Ok(start.elapsed().as_secs_f64())
}

async fn run_client(
    host: String,
    base_port: u16,
    lanes: u16,
    in_path: String,
    cert_path: String,
) -> Result<()> {
    let data = std::fs::read(&in_path)?;

    let cert_bytes = std::fs::read(&cert_path)?;
    let cert = CertificateDer::from(cert_bytes);
    let mut roots = rustls::RootCertStore::empty();
    roots.add(cert)?;
    let mut client_config = ClientConfig::with_root_certificates(Arc::new(roots))?;
    client_config.transport_config(Arc::new(transport()));

    // Contiguous shards, matching the Fuse harness's split.
    let total = data.len();
    let shard_len = total.div_ceil(lanes as usize);

    let overall = Instant::now();
    let mut tasks = Vec::new();
    for i in 0..lanes {
        let off = std::cmp::min(i as usize * shard_len, total);
        let end = std::cmp::min(off + shard_len, total);
        tasks.push(tokio::spawn(send_lane(
            host.clone(),
            base_port + i,
            data[off..end].to_vec(),
            client_config.clone(),
        )));
    }
    let mut slowest = 0.0f64;
    for t in tasks {
        slowest = slowest.max(t.await??);
    }
    let wall = overall.elapsed().as_secs_f64();

    println!(
        "SEND bytes={} lanes={} elapsed={:.4} throughput={:.1} MB/s wall={:.4}",
        total,
        lanes,
        slowest,
        (total as f64 / (1024.0 * 1024.0)) / slowest,
        wall
    );
    Ok(())
}

#[tokio::main]
async fn main() -> Result<()> {
    rustls::crypto::ring::default_provider()
        .install_default()
        .map_err(|_| anyhow!("failed to install rustls crypto provider"))?;

    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!(
            "usage:\n  {0} recv <base-port> <lanes> <out-file> <cert-out>\n  {0} send <host> <base-port> <lanes> <in-file> <cert-in>",
            args[0]
        );
        std::process::exit(2);
    }

    match args[1].as_str() {
        "recv" if args.len() >= 6 => {
            run_server(
                args[2].parse()?,
                args[3].parse()?,
                args[4].clone(),
                args[5].clone(),
            )
            .await
        }
        "send" if args.len() >= 7 => {
            run_client(
                args[2].clone(),
                args[3].parse()?,
                args[4].parse()?,
                args[5].clone(),
                args[6].clone(),
            )
            .await
        }
        _ => {
            eprintln!("bad arguments");
            std::process::exit(2);
        }
    }
}
