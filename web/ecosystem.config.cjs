module.exports = {
  apps: [
    {
      name: 'esphome-uptime',
      script: 'server/index.js',
      cwd: '/home/ubuntu/apps/esphome-uptime',
      instances: 1,
      exec_mode: 'fork',
      autorestart: true,
      max_restarts: 50,
      restart_delay: 2000,
      max_memory_restart: '300M',
      time: true,
      out_file: '/home/ubuntu/.pm2/logs/esphome-uptime-out.log',
      error_file: '/home/ubuntu/.pm2/logs/esphome-uptime-err.log',
    },
  ],
};
