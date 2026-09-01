// ==================== 模块：数据库依赖 ====================
const Database = require('better-sqlite3');

// ==================== 模块：用户表结构 ====================
// 功能：让生产环境和测试环境使用完全相同的 users 表结构。
const userSchema = [
    'CREATE TABLE IF NOT EXISTS users (',
    '    id INTEGER PRIMARY KEY AUTOINCREMENT,',
    '    username TEXT NOT NULL UNIQUE,',
    '    password TEXT NOT NULL,',
    "    created_at TEXT DEFAULT (datetime('now'))",
    ');'
].join('\n');

// ==================== 模块：数据库工厂 ====================
// 输入：SQLite 数据库文件路径。
// 输出：已完成 users 表初始化、由调用方负责 close() 的数据库连接。
// 边界：require 本模块不会再隐式创建 auto.db，测试可传入自己的临时路径。
function createDatabase(databasePath) {
    if (typeof databasePath !== 'string' || databasePath.trim().length === 0) {
        throw new TypeError('database_path_invalid');
    }

    const db = new Database(databasePath);
    db.exec(userSchema);
    return db;
}

// ==================== 模块：数据库工厂导出 ====================
module.exports = {createDatabase};
