'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');

const {
    closeUsersAndAssertServerAlive,
    connectAndAuthenticateUsers,
    createChatEnvironment,
    createUsers,
    relayRing
} = require('./e2e_chat_support');

// W13 的 10 客户端基线：每位用户都必须经过真实注册、登录、TCP introspection，
// 再向环上的下一位用户发送一条消息，避免“只连上但没有走业务路径”的假成功。
test('十个真实客户端认证、环形聊天并关闭后 ChatServer 仍保持可用', {timeout: 120000}, async t => {
    const environment = await createChatEnvironment(t);
    const users = await createUsers(environment, 10, 'ten');
    const statistics = {
        connected: 0,
        authenticated: 0,
        sent: 0,
        acknowledged: 0,
        forwarded: 0,
        delivered: 0,
        closed: 0
    };

    await connectAndAuthenticateUsers(environment, users, statistics);
    await relayRing(environment, users, statistics);
    await closeUsersAndAssertServerAlive(environment, users, statistics);

    // 同一批客户端的每个计数必须守恒；少一项都说明某个异步阶段被静默跳过。
    assert.deepEqual(statistics, {
        connected: 10,
        authenticated: 10,
        sent: 10,
        acknowledged: 10,
        forwarded: 10,
        delivered: 10,
        closed: 10
    });
    t.diagnostic(
        'scenario=ten_client_ring connected=10 authenticated=10 sent=10 '
        + 'acknowledged=10 forwarded=10 delivered=10 closed=10'
    );
});
