'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const net = require('node:net');
const path = require('node:path');

const {
    CHAT_FRAME_TYPE,
    ERROR_FRAME_TYPE,
    closeUsersAndAssertServerAlive,
    connectAndAuthenticateUsers,
    createChatEnvironment,
    createUsers,
    probeTcpPort,
    readJsonFrame,
    withDeadline
} = require('./e2e_chat_support');

const PROJECT_ROOT = path.resolve(__dirname, '..', '..');
const COMPOSE_FILE = path.join(PROJECT_ROOT, 'docker-compose.yml');
const MYSQL_OPERATION_TIMEOUT_MS = 10000;

function loadComposeMySqlConfig() {
    // 只在进程内读取本机私有 .env，既不写入日志，也不把密码放进 Git 或命令行参数。
    require('dotenv').config({path: path.join(PROJECT_ROOT, '.env'), quiet: true});
    const config = {
        host: '127.0.0.1',
        port: Number(process.env.CHATHUB_MYSQL_HOST_PORT || '3307'),
        username: process.env.CHATHUB_MYSQL_USER,
        password: process.env.CHATHUB_MYSQL_PASSWORD,
        root_password_present: typeof process.env.CHATHUB_MYSQL_ROOT_PASSWORD === 'string'
            && process.env.CHATHUB_MYSQL_ROOT_PASSWORD.length > 0
    };
    if (!Number.isInteger(config.port) || config.port < 1 || config.port > 65535
        || typeof config.username !== 'string' || !/^[A-Za-z0-9_]+$/.test(config.username)
        || typeof config.password !== 'string' || config.password.length === 0
        || !config.root_password_present) {
        throw new Error('w13_mysql_private_env_missing_or_invalid');
    }
    return config;
}

function runCommand(executable, arguments_list, cwd, label) {
    return new Promise((resolve, reject) => {
        const child = spawn(executable, arguments_list, {
            cwd,
            env: process.env,
            stdio: ['ignore', 'pipe', 'pipe']
        });
        let output = '';
        child.stdout.setEncoding('utf8');
        child.stderr.setEncoding('utf8');
        child.stdout.on('data', chunk => { output += chunk; });
        child.stderr.on('data', chunk => { output += chunk; });
        child.once('error', () => reject(new Error(`${label}_spawn_failed`)));
        child.once('exit', code => {
            if (code === 0) {
                resolve(output);
            } else {
                // SQL、命令行和子进程输出都可能含环境细节；测试对外只暴露稳定阶段错误。
                reject(new Error(`${label}_failed:${code ?? 'null'}`));
            }
        });
    });
}

async function runComposeMySqlSql(sql, label) {
    // 密码在容器内由 $MYSQL_ROOT_PASSWORD 展开，主机命令参数和测试诊断都不携带密码。
    return withDeadline(runCommand('docker', [
        'compose', '-f', COMPOSE_FILE, '--project-directory', PROJECT_ROOT,
        'exec', '-T', 'mysql', 'sh', '-lc',
        'exec mysql -uroot -p"$MYSQL_ROOT_PASSWORD" -e "$1"',
        'w13_mysql_test', sql
    ], PROJECT_ROOT, label), MYSQL_OPERATION_TIMEOUT_MS, label);
}

async function createDedicatedDatabase(database_name, mysql_config) {
    const identifier = `\`${database_name}\``;
    await runComposeMySqlSql(
        `CREATE DATABASE ${identifier}; GRANT ALL PRIVILEGES ON ${identifier}.* `
        + `TO '${mysql_config.username}'@'%'; FLUSH PRIVILEGES;`,
        'mysql_create_dedicated_database'
    );
}

async function dropDedicatedDatabase(database_name, mysql_config) {
    const identifier = `\`${database_name}\``;
    await runComposeMySqlSql(
        `REVOKE ALL PRIVILEGES ON ${identifier}.* FROM '${mysql_config.username}'@'%'; `
        + `DROP DATABASE IF EXISTS ${identifier}; FLUSH PRIVILEGES;`,
        'mysql_drop_dedicated_database'
    );
}

function startMysqlProxy(target_host, target_port) {
    return new Promise((resolve, reject) => {
        const sockets = new Set();
        let stopped = false;
        const server = net.createServer(client_socket => {
            const upstream_socket = net.createConnection({host: target_host, port: target_port});
            sockets.add(client_socket);
            sockets.add(upstream_socket);

            const destroyPair = () => {
                client_socket.destroy();
                upstream_socket.destroy();
            };
            client_socket.once('error', destroyPair);
            upstream_socket.once('error', destroyPair);
            client_socket.once('close', () => sockets.delete(client_socket));
            upstream_socket.once('close', () => sockets.delete(upstream_socket));
            client_socket.pipe(upstream_socket);
            upstream_socket.pipe(client_socket);
        });

        server.once('error', reject);
        server.listen(0, '127.0.0.1', () => {
            const address = server.address();
            if (!address || typeof address === 'string') {
                server.close(() => reject(new Error('mysql_proxy_address_invalid')));
                return;
            }
            resolve({
                port: address.port,
                async stop() {
                    // 故障注入会主动停止一次，context cleanup 会再次调用；停止必须幂等。
                    if (stopped) {
                        return;
                    }
                    stopped = true;
                    for (const socket of sockets) {
                        socket.destroy();
                    }
                    await new Promise((resolve_stop, reject_stop) => {
                        server.close(error => error ? reject_stop(error) : resolve_stop());
                    });
                }
            });
        });
    });
}

test('MySQL 连接在运行中失效时，消息不确认也不转发', {timeout: 90000}, async t => {
    const mysql_config = loadComposeMySqlConfig();
    assert.equal(await probeTcpPort(mysql_config.port), 'OPEN', 'compose_mysql_port_not_open');

    const environment = await createChatEnvironment(t, {
        prepare_chat_server: async setup => {
            // 数据库名只由固定前缀和 UUID 派生；清理仅触及本次专用 schema。
            const database_name = `w13_fail_${setup.run_id.slice(0, 12)}`;
            await createDedicatedDatabase(database_name, mysql_config);
            setup.context.defer(async () => dropDedicatedDatabase(database_name, mysql_config));

            const mysql_proxy = await startMysqlProxy(mysql_config.host, mysql_config.port);
            setup.context.defer(async () => mysql_proxy.stop());
            setup.mysql_proxy = mysql_proxy;

            return {
                arguments: [
                    '--port', String(setup.chat_server_port),
                    '--storage-backend', 'mysql',
                    '--mysql-host', mysql_config.host,
                    '--mysql-port', String(mysql_proxy.port),
                    '--mysql-username', mysql_config.username,
                    '--mysql-database', database_name,
                    '--auth-timeout-ms', '5000'
                ],
                // ChatServer 只从进程环境取得应用密码；该值不会进入参数、日志或断言文本。
                environment: {CHATHUB_MYSQL_PASSWORD: mysql_config.password}
            };
        }
    });
    const [alice, bob] = await createUsers(environment, 2, 'mysql');
    const statistics = {
        connected: 0,
        authenticated: 0,
        attempted: 0,
        database_errors: 0,
        forwarded: 0,
        acknowledged: 0,
        closed: 0
    };
    await connectAndAuthenticateUsers(environment, [alice, bob], statistics);

    // 仅切断 ChatServer 的专用代理连接；Compose MySQL 继续健康，因此不会影响其他开发服务。
    await environment.mysql_proxy.stop();
    const local_id = `w13_${environment.run_id.slice(0, 12)}_mysql_write_failure`;
    alice.client.writeFrame(CHAT_FRAME_TYPE, JSON.stringify({
        to: bob.username,
        local_id,
        content: 'must-not-be-acknowledged-after-mysql-loss',
        send_at: new Date().toISOString()
    }));
    statistics.attempted += 1;

    const error_body = await readJsonFrame(alice.client, ERROR_FRAME_TYPE, 'mysql_write_failure_error');
    assert.equal(error_body.scope, 'chat');
    assert.equal(error_body.code, 'database_write_failed');
    assert.equal(error_body.local_id, local_id);
    statistics.database_errors += 1;

    // 数据库提交未成功时，接收者既不能得到 chat，也不能把失败伪装成已转发。
    await assert.rejects(
        () => bob.client.readFrame(250),
        error => error instanceof Error && error.message === 'tcp_frame_read_timeout'
    );
    assert.equal(environment.chat_process.child.exitCode, null, 'chat_server_exited_after_mysql_loss');
    assert.equal(await probeTcpPort(environment.chat_server_port), 'OPEN',
        'chat_server_not_listening_after_mysql_loss');

    await closeUsersAndAssertServerAlive(environment, [alice, bob], statistics);
    assert.deepEqual(statistics, {
        connected: 2,
        authenticated: 2,
        attempted: 1,
        database_errors: 1,
        forwarded: 0,
        acknowledged: 0,
        closed: 2
    });
    t.diagnostic(
        'scenario=mysql_write_failure connected=2 authenticated=2 attempted=1 '
        + 'database_errors=1 acknowledged=0 forwarded=0 closed=2'
    );
});
