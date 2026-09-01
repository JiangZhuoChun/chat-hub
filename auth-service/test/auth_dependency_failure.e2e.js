'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

const {
    AUTH_FRAME_TYPE,
    AUTH_SERVICE_PORT,
    closeFrameClient,
    closeUsersAndAssertServerAlive,
    connectAndAuthenticateUsers,
    connectFrameClient,
    createChatEnvironment,
    createUsers,
    readAuthFailure,
    relayAndConfirm,
    stopProcess,
    waitForPortClosed,
    withDeadline
} = require('./e2e_chat_support');

test('Auth introspection 不可用时新认证 fail-closed，旧会话仍可聊天', {timeout: 60000}, async t => {
    const environment = await createChatEnvironment(t);
    const [alice, carol, bob] = await createUsers(environment, 3, 'auth');
    const statistics = {
        connected: 0,
        authenticated: 0,
        new_auth_attempts: 0,
        dependency_rejections: 0,
        sent: 0,
        acknowledged: 0,
        forwarded: 0,
        delivered: 0,
        closed: 0
    };

    // Alice 和 Carol 已经完成认证；后续停止 Auth 不应倒推使这两条旧 Session 失效。
    await connectAndAuthenticateUsers(environment, [alice, carol], statistics);
    await stopProcess(environment.auth_process, 'auth_process_stop_for_dependency_failure');
    await waitForPortClosed(AUTH_SERVICE_PORT, 'auth_port_closed_for_dependency_failure');

    // Bob 是 Auth 停止后的新连接。它拿着此前真实签发的 token，ChatServer 仍必须重新 introspect，
    // 因而只能得到 dependency_unavailable，而不能因 token 形状正确就放行。
    bob.client = await withDeadline(
        connectFrameClient(environment.chat_server_port), 5000, 'bob_connect_after_auth_stop'
    );
    environment.context.defer(async () => closeFrameClient(bob.client, 'bob_dependency_failure_cleanup'));
    statistics.connected += 1;
    bob.client.writeFrame(AUTH_FRAME_TYPE, bob.token);
    statistics.new_auth_attempts += 1;
    const failure = await readAuthFailure(bob.client, 'bob_auth_dependency_failure');
    assert.equal(failure.code, 'authentication_dependency_unavailable');
    statistics.dependency_rejections += 1;

    // 已建立的两条会话不重新请求 Auth；它们仍需完成完整的 ACK、转发与送达路径。
    await relayAndConfirm({
        sender: alice,
        recipient: carol,
        run_id: environment.run_id,
        sequence: 'alice_to_carol_after_auth_dependency_failure',
        statistics
    });

    await closeFrameClient(bob.client, 'bob_dependency_failure_close');
    await closeUsersAndAssertServerAlive(environment, [alice, carol], statistics);
    assert.deepEqual(statistics, {
        connected: 3,
        authenticated: 2,
        new_auth_attempts: 1,
        dependency_rejections: 1,
        sent: 1,
        acknowledged: 1,
        forwarded: 1,
        delivered: 1,
        closed: 2
    });
    t.diagnostic(
        'scenario=auth_dependency_failure connected=3 authenticated=2 new_auth_attempts=1 '
        + 'dependency_rejections=1 sent=1 acknowledged=1 forwarded=1 delivered=1 closed=2'
    );
});
