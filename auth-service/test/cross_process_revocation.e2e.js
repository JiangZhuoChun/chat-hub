'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const {existsSync} = require('node:fs');
const {mkdtemp, rm} = require('node:fs/promises');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const {randomUUID} = require('node:crypto');
const jwt = require('jsonwebtoken');

const {createRedisClient, connectRedis, closeRedis} = require('../src/redis_client');
const {requestJson} = require('./http_test_helpers');

const REDIS_URL = process.env.CHATHUB_REDIS_TEST_URL;
const AUTH_SERVICE_PORT = 3000;
const FRAME_MAGIC = 0x4348;
const PROTOCOL_VERSION = 1;
const AUTH_FRAME_TYPE = 5;
const PING_FRAME_TYPE = 2;
const PONG_FRAME_TYPE = 3;
const AUTH_RESPONSE_FRAME_TYPE = 5;
const ERROR_FRAME_TYPE = 4;
const ONLINE_USERS_FRAME_TYPE = 8;
const MAX_FRAME_BODY_LENGTH = 2048;
const PROCESS_STARTUP_TIMEOUT_MS = 10000;
const PROCESS_EXIT_TIMEOUT_MS = 5000;
const TCP_OPERATION_TIMEOUT_MS = 5000;

if (typeof REDIS_URL !== 'string' || REDIS_URL.trim().length === 0) {
    throw new Error('CHATHUB_REDIS_TEST_URL_missing');
}

function probeTcpPort(port, timeout_ms = 1000) {
    return new Promise(resolve => {
        const socket = net.createConnection({
            host: '127.0.0.1',
            port
        });
        let settled = false;
        const timeout_id = setTimeout(() => finish('TIMEOUT'), timeout_ms);

        const finish = result => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timeout_id);
            socket.destroy();
            resolve(result);
        };

        socket.once('connect', () => finish('OPEN'));
        socket.once('error', error => finish(error.code || 'ERROR'));
    });
}

async function waitForPortClosed(port, timeout_ms = PROCESS_EXIT_TIMEOUT_MS) {
    const deadline = Date.now() + timeout_ms;
    while (Date.now() <= deadline) {
        if (await probeTcpPort(port) !== 'OPEN') {
            return;
        }

        const remaining_ms = deadline - Date.now();
        if (remaining_ms <= 0) {
            break;
        }

        await new Promise(resolve => {
            setTimeout(resolve, Math.min(50, remaining_ms));
        });
    }

    throw new Error(`tcp_port_still_open:${port}`);
}

function acquireUnusedPort() {
    return new Promise((resolve, reject) => {
        const server = net.createServer();
        server.once('error', reject);
        server.listen(0, '127.0.0.1', () => {
            const address = server.address();
            if (!address || typeof address === 'string') {
                server.close(() => reject(new Error('unused_port_invalid')));
                return;
            }

            server.close(error => {
                if (error) {
                    reject(error);
                    return;
                }
                resolve(address.port);
            });
        });
    });
}

function startProcess(executable, arguments_list, {cwd, env}) {
    const child = spawn(executable, arguments_list, {
        cwd,
        env,
        stdio: ['ignore', 'pipe', 'pipe']
    });

    let stdout = '';
    let stderr = '';
    let spawn_error;

    child.stdout.setEncoding('utf8');
    child.stderr.setEncoding('utf8');
    child.stdout.on('data', chunk => {
        stdout += chunk;
    });
    child.stderr.on('data', chunk => {
        stderr += chunk;
    });
    child.once('error', error => {
        spawn_error = error;
    });

    const exit = new Promise(resolve => {
        child.once('exit', (code, signal) => {
            resolve({code, signal});
        });
    });

    return {
        child,
        exit,
        output: () => `${stdout}\n${stderr}`,
        spawn_error: () => spawn_error
    };
}

async function waitForProcessOutput(process_handle, marker,
                                    timeout_ms = PROCESS_STARTUP_TIMEOUT_MS) {
    const deadline = Date.now() + timeout_ms;
    while (Date.now() <= deadline) {
        if (process_handle.output().includes(marker)) {
            return;
        }

        const exit_state = await Promise.race([
            process_handle.exit.then(state => ({kind: 'exit', state})),
            new Promise(resolve => {
                const remaining_ms = deadline - Date.now();
                setTimeout(
                    () => resolve({kind: 'timeout'}),
                    Math.max(1, Math.min(50, remaining_ms))
                );
            })
        ]);

        if (exit_state.kind === 'exit') {
            const spawn_error = process_handle.spawn_error();
            throw new Error(
                `${marker}_process_exited:${exit_state.state.code ?? 'null'}:`
                + `${spawn_error?.code || 'no_spawn_error'}`
            );
        }
    }

    throw new Error(`${marker}_startup_timeout`);
}

async function waitForProcessExit(process_handle,
                                  timeout_ms = PROCESS_EXIT_TIMEOUT_MS) {
    let timeout_id;
    try {
        return await Promise.race([
            process_handle.exit,
            new Promise((_, reject) => {
                timeout_id = setTimeout(
                    () => reject(new Error('process_exit_timeout')),
                    timeout_ms
                );
            })
        ]);
    } finally {
        clearTimeout(timeout_id);
    }
}

async function stopProcess(process_handle) {
    if (!process_handle || process_handle.child.exitCode !== null) {
        return;
    }

    process_handle.child.kill();
    await waitForProcessExit(process_handle);
}

function makeFrame(type, body) {
    const body_buffer = Buffer.from(body, 'utf8');
    assert.ok(body_buffer.length <= MAX_FRAME_BODY_LENGTH);

    const frame = Buffer.alloc(8 + body_buffer.length);
    frame.writeUInt16BE(FRAME_MAGIC, 0);
    frame.writeUInt8(PROTOCOL_VERSION, 2);
    frame.writeUInt8(type, 3);
    frame.writeUInt32BE(body_buffer.length, 4);
    body_buffer.copy(frame, 8);
    return frame;
}

function createFrameClient(socket) {
    let received = Buffer.alloc(0);
    let pending_read = null;
    let pending_timeout_id = null;
    let socket_error = null;

    const rejectPendingRead = error => {
        if (!pending_read) {
            return;
        }
        const current = pending_read;
        pending_read = null;
        clearTimeout(pending_timeout_id);
        pending_timeout_id = null;
        current.reject(error);
    };

    const readOneFrame = () => {
        if (received.length < 8) {
            return null;
        }

        const magic = received.readUInt16BE(0);
        const version = received.readUInt8(2);
        const type = received.readUInt8(3);
        const body_length = received.readUInt32BE(4);
        if (magic !== FRAME_MAGIC || version !== PROTOCOL_VERSION
            || body_length > MAX_FRAME_BODY_LENGTH) {
            throw new Error('invalid_chat_frame');
        }

        const frame_length = 8 + body_length;
        if (received.length < frame_length) {
            return null;
        }

        const body = received.subarray(8, frame_length).toString('utf8');
        received = received.subarray(frame_length);
        return {type, body};
    };

    const tryResolveRead = () => {
        if (!pending_read) {
            return;
        }

        let frame;
        try {
            frame = readOneFrame();
        } catch (error) {
            rejectPendingRead(error);
            return;
        }
        if (!frame) {
            return;
        }

        const current = pending_read;
        pending_read = null;
        clearTimeout(pending_timeout_id);
        pending_timeout_id = null;
        current.resolve(frame);
    };

    socket.on('data', chunk => {
        received = Buffer.concat([received, chunk]);
        tryResolveRead();
    });
    socket.once('error', error => {
        socket_error = error;
        rejectPendingRead(error);
    });
    socket.once('close', () => {
        rejectPendingRead(socket_error || new Error('tcp_socket_closed'));
    });

    return {
        writeFrame(type, body) {
            if (socket.destroyed) {
                throw new Error('tcp_socket_destroyed');
            }
            socket.write(makeFrame(type, body));
        },
        readFrame(timeout_ms = TCP_OPERATION_TIMEOUT_MS) {
            if (pending_read) {
                return Promise.reject(new Error('concurrent_frame_read'));
            }

            return new Promise((resolve, reject) => {
                pending_read = {resolve, reject};
                pending_timeout_id = setTimeout(() => {
                    rejectPendingRead(new Error('tcp_frame_read_timeout'));
                }, timeout_ms);
                tryResolveRead();
            });
        },
        waitForClose(timeout_ms = TCP_OPERATION_TIMEOUT_MS) {
            if (socket.destroyed) {
                return Promise.resolve();
            }

            return new Promise((resolve, reject) => {
                const timeout_id = setTimeout(
                    () => reject(new Error('tcp_close_timeout')),
                    timeout_ms
                );
                socket.once('close', () => {
                    clearTimeout(timeout_id);
                    resolve();
                });
            });
        },
        destroy() {
            socket.destroy();
        }
    };
}

function connectFrameClient(port, timeout_ms = TCP_OPERATION_TIMEOUT_MS) {
    return new Promise((resolve, reject) => {
        const socket = net.createConnection({
            host: '127.0.0.1',
            port
        });
        let settled = false;
        const timeout_id = setTimeout(() => finish(null, new Error('tcp_connect_timeout')), timeout_ms);
        const finish = (value, error) => {
            if (settled) {
                return;
            }
            settled = true;
            clearTimeout(timeout_id);
            socket.removeListener('error', on_error);
            if (error) {
                socket.destroy();
                reject(error);
                return;
            }
            resolve(createFrameClient(socket));
        };
        const on_error = error => finish(null, error);

        socket.once('connect', () => finish(socket, null));
        socket.once('error', on_error);
    });
}

async function authenticate(frame_client, token) {
    frame_client.writeFrame(AUTH_FRAME_TYPE, token);

    const auth_frame = await frame_client.readFrame();
    assert.equal(auth_frame.type, AUTH_RESPONSE_FRAME_TYPE);
    assert.equal(JSON.parse(auth_frame.body).ok, true);

    const online_users_frame = await frame_client.readFrame();
    assert.equal(online_users_frame.type, ONLINE_USERS_FRAME_TYPE);
}

async function expectAuthenticationRejected(port, token) {
    const frame_client = await connectFrameClient(port);
    try {
        frame_client.writeFrame(AUTH_FRAME_TYPE, token);
        const error_frame = await frame_client.readFrame();
        assert.equal(error_frame.type, ERROR_FRAME_TYPE);
        assert.equal(JSON.parse(error_frame.body).code, 'authentication_rejected');
        await frame_client.waitForClose();
    } finally {
        frame_client.destroy();
    }
}

test('当真实 Auth 与 ChatServer 共享撤销状态时，logout 后新连接应拒绝且旧连接保持',
     {timeout: 60000}, async t => {
         const chat_server_executable = process.env.CHATHUB_CHAT_SERVER_EXECUTABLE
             ? path.resolve(process.env.CHATHUB_CHAT_SERVER_EXECUTABLE)
             : path.resolve(__dirname, '..', '..', 'cmake-build-debug-mysql',
                 'chat-server', 'chat-server.exe');
         assert.ok(
             existsSync(chat_server_executable),
             `chat_server_executable_missing:${chat_server_executable}`
         );
         assert.equal(
             await probeTcpPort(AUTH_SERVICE_PORT),
             'ECONNREFUSED',
             'Auth Service 的固定 3000 端口必须在测试前空闲'
         );

         const run_id = randomUUID().replace(/-/g, '');
         const key_prefix = `chathub:e2e:${run_id}`;
         const username = `e2e_${run_id.slice(0, 12)}`;
         const password = 'password123';
         const secret_key = `e2e-secret-${run_id}`;
         const internal_service_key = `e2e-internal-${run_id}`;
         const auth_directory = await mkdtemp(
             path.join(os.tmpdir(), 'chathub-auth-e2e-')
         );
         const chat_directory = await mkdtemp(
             path.join(os.tmpdir(), 'chathub-chat-e2e-')
         );
         const chat_database_path = path.join(chat_directory, 'chat.db');
         const chat_server_port = await acquireUnusedPort();
         const redis_client = createRedisClient({
             url: REDIS_URL.trim(),
             connectTimeoutMs: 2000,
             maxReconnectAttempts: 0,
             reconnectDelayMs: 0
         });

         let auth_process;
         let chat_process;
         let frame_client;
         let token;
         let revoked_key;

         t.after(async () => {
             if (frame_client) {
                 frame_client.destroy();
             }
             await stopProcess(chat_process);
             await stopProcess(auth_process);
             if (chat_process) {
                 await waitForPortClosed(chat_server_port);
             }
             if (auth_process) {
                 await waitForPortClosed(AUTH_SERVICE_PORT);
             }

             if (redis_client.isOpen && redis_client.isReady && revoked_key) {
                 await redis_client.del(revoked_key);
             }
             await closeRedis(redis_client);
             await rm(auth_directory, {recursive: true, force: true});
             await rm(chat_directory, {recursive: true, force: true});
         });

         await connectRedis(redis_client);

         auth_process = startProcess(
             process.execPath,
             [path.resolve(__dirname, '..', 'src', 'server.js')],
             {
                 cwd: auth_directory,
                 env: {
                     ...process.env,
                     CHATHUB_REDIS_URL: REDIS_URL.trim(),
                     SECRET_KEY: secret_key,
                     CHATHUB_AUTH_INTERNAL_SERVICE_KEY: internal_service_key,
                     CHATHUB_REDIS_KEY_PREFIX: key_prefix,
                     CHATHUB_LOGIN_USER_LIMIT: '5',
                     CHATHUB_LOGIN_IP_LIMIT: '20',
                     CHATHUB_LOGIN_WINDOW_SECONDS: '60'
                 }
             }
         );
         await waitForProcessOutput(
             auth_process,
             'component=auth phase=startup event=listening port=3000'
         );

         const registration = await requestJson(
             `http://127.0.0.1:${AUTH_SERVICE_PORT}`,
             '/register',
             {
                 method: 'POST',
                 headers: {'Content-Type': 'application/json'},
                 body: JSON.stringify({username, password})
             }
         );
         assert.equal(registration.status, 201);

         const login = await requestJson(
             `http://127.0.0.1:${AUTH_SERVICE_PORT}`,
             '/login',
             {
                 method: 'POST',
                 headers: {'Content-Type': 'application/json'},
                 body: JSON.stringify({username, password})
             }
         );
         assert.equal(login.status, 200);
         assert.equal(login.body.username, username);
         assert.equal(typeof login.body.token, 'string');
         token = login.body.token;

         // 这里只用 decode 定位本次测试的 Redis marker；认证决定仍由真实 Auth/ChatServer 链路完成。
         const claims = jwt.decode(token);
         assert.ok(claims && typeof claims === 'object');
         assert.equal(claims.username, username);
         assert.equal(typeof claims.jti, 'string');
         assert.ok(Number.isSafeInteger(claims.exp));
         revoked_key = `${key_prefix}:revoked:jti:${claims.jti}`;

         chat_process = startProcess(
             chat_server_executable,
             [
                 '--port', String(chat_server_port),
                 '--database-path', chat_database_path,
                 '--auth-timeout-ms', '5000'
             ],
             {
                 cwd: chat_directory,
                 env: {
                     ...process.env,
                     CHATHUB_AUTH_INTROSPECTION_URL:
                         `http://127.0.0.1:${AUTH_SERVICE_PORT}/internal/auth/introspect`,
                     CHATHUB_AUTH_INTERNAL_SERVICE_KEY: internal_service_key
                 }
             }
         );
         await waitForProcessOutput(chat_process, 'server_started');
         assert.equal(await probeTcpPort(chat_server_port), 'OPEN');

         frame_client = await connectFrameClient(chat_server_port);
         await authenticate(frame_client, token);

         const before_logout = await requestJson(
             `http://127.0.0.1:${AUTH_SERVICE_PORT}`,
             '/me',
             {headers: {Authorization: `Bearer ${token}`}}
         );
         assert.equal(before_logout.status, 200);
         assert.equal(before_logout.body.username, username);

         const logout = await requestJson(
             `http://127.0.0.1:${AUTH_SERVICE_PORT}`,
             '/logout',
             {
                 method: 'POST',
                 headers: {Authorization: `Bearer ${token}`}
             }
         );
         assert.equal(logout.status, 200);

         const revoked_ttl = await redis_client.ttl(revoked_key);
         const remaining_lifetime = claims.exp - Math.floor(Date.now() / 1000);
         assert.ok(revoked_ttl > 0);
         assert.ok(revoked_ttl <= remaining_lifetime);

         const after_logout = await requestJson(
             `http://127.0.0.1:${AUTH_SERVICE_PORT}`,
             '/me',
             {headers: {Authorization: `Bearer ${token}`}}
         );
         assert.equal(after_logout.status, 401);
         assert.equal(after_logout.body.code, 'authentication_rejected');

         frame_client.writeFrame(PING_FRAME_TYPE, 'old-session-alive');
         const old_session_pong = await frame_client.readFrame();
         assert.equal(old_session_pong.type, PONG_FRAME_TYPE);
         assert.equal(old_session_pong.body, 'old-session-alive');

         await expectAuthenticationRejected(chat_server_port, token);

         frame_client.destroy();
         frame_client = null;
         await stopProcess(chat_process);
         chat_process = null;
         await waitForPortClosed(chat_server_port);
         await stopProcess(auth_process);
         auth_process = null;
         await waitForPortClosed(AUTH_SERVICE_PORT);

         auth_process = startProcess(
             process.execPath,
             [path.resolve(__dirname, '..', 'src', 'server.js')],
             {
                 cwd: auth_directory,
                 env: {
                     ...process.env,
                     CHATHUB_REDIS_URL: REDIS_URL.trim(),
                     SECRET_KEY: secret_key,
                     CHATHUB_AUTH_INTERNAL_SERVICE_KEY: internal_service_key,
                     CHATHUB_REDIS_KEY_PREFIX: key_prefix,
                     CHATHUB_LOGIN_USER_LIMIT: '5',
                     CHATHUB_LOGIN_IP_LIMIT: '20',
                     CHATHUB_LOGIN_WINDOW_SECONDS: '60'
                 }
             }
         );
         await waitForProcessOutput(
             auth_process,
             'component=auth phase=startup event=listening port=3000'
         );

         const after_auth_restart = await requestJson(
             `http://127.0.0.1:${AUTH_SERVICE_PORT}`,
             '/me',
             {headers: {Authorization: `Bearer ${token}`}}
         );
         assert.equal(after_auth_restart.status, 401);
         assert.equal(after_auth_restart.body.code, 'authentication_rejected');

         chat_process = startProcess(
             chat_server_executable,
             [
                 '--port', String(chat_server_port),
                 '--database-path', chat_database_path,
                 '--auth-timeout-ms', '5000'
             ],
             {
                 cwd: chat_directory,
                 env: {
                     ...process.env,
                     CHATHUB_AUTH_INTROSPECTION_URL:
                         `http://127.0.0.1:${AUTH_SERVICE_PORT}/internal/auth/introspect`,
                     CHATHUB_AUTH_INTERNAL_SERVICE_KEY: internal_service_key
                 }
             }
         );
         await waitForProcessOutput(chat_process, 'server_started');
         assert.equal(await probeTcpPort(chat_server_port), 'OPEN');
         await expectAuthenticationRejected(chat_server_port, token);
     });
