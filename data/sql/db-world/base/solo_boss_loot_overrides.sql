-- mod-solo-boss-loot: boss chests, extra bosses and per-item exceptions
--
-- SourceType  Entry from            Mode 1 (include)                                  Mode 0 (exclude)
-- 0           creature_template     treat this creature as a boss                     never treat it as a boss
-- 1           gameobject_template   boss chest: its loot gets the extra boss items    (same as having no row)
-- 2           item_template         always add it when a boss drops it (*)            never add it
--
-- (*) skips the "only bosses drop it", quality and shared-pool checks.
--     The class, owned-item and already-learned checks still apply.
--
-- Changes take effect on server start or `.reload config`.
-- Rows use INSERT IGNORE, so your own edits survive if this file is applied again.

CREATE TABLE IF NOT EXISTS `solo_boss_loot_overrides` (
    `SourceType` TINYINT UNSIGNED NOT NULL COMMENT '0 = creature, 1 = gameobject (boss chest), 2 = item',
    `Entry`      INT UNSIGNED     NOT NULL,
    `Mode`       TINYINT UNSIGNED NOT NULL DEFAULT 1 COMMENT '1 = include, 0 = exclude',
    `Comment`    VARCHAR(255)     NOT NULL DEFAULT '',
    PRIMARY KEY (`SourceType`, `Entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

INSERT IGNORE INTO `solo_boss_loot_overrides` (`SourceType`, `Entry`, `Mode`, `Comment`) VALUES
-- Classic
(1, 179703, 1, 'Molten Core - Cache of the Firelord (Majordomo Executus)'),
(1, 169243, 1, 'Blackrock Depths - Chest of The Seven'),
(1, 181074, 1, 'Blackrock Depths - Arena Spoils (Ring of Law)'),
(1, 181083, 1, 'Stratholme - Sothos and Jarien''s Heirlooms'),
(1, 179564, 1, 'Dire Maul - Gordok Tribute'),

-- The Burning Crusade
(1, 185168, 1, 'Hellfire Ramparts - Reinforced Fel Iron Chest (normal)'),
(1, 185169, 1, 'Hellfire Ramparts - Reinforced Fel Iron Chest'),
(1, 184465, 1, 'The Mechanar - Cache of the Legion'),
(1, 184849, 1, 'The Mechanar - Cache of the Legion'),
(1, 187372, 1, 'Sethekk Halls - The Talon King''s Coffer'),
(1, 187892, 1, 'The Slave Pens - Ice Chest (Ahune)'),
(1, 188192, 1, 'The Slave Pens - Ice Chest (Ahune)'),
(1, 185119, 1, 'Karazhan - Dust Covered Chest (Chess Event)'),
(1, 186648, 1, 'Zul''Aman - Tanzar''s Trunk (timed event)'),
(1, 186667, 1, 'Zul''Aman - Kraz''s Package (timed event)'),
(1, 186672, 1, 'Zul''Aman - Ashli''s Bag (timed event)'),
(1, 187021, 1, 'Zul''Aman - Harkor''s Satchel (timed event)'),

-- Wrath of the Lich King dungeons
(1, 190586, 1, 'Halls of Stone - Tribunal Chest (normal)'),
(1, 193996, 1, 'Halls of Stone - Tribunal Chest (heroic)'),
(1, 191349, 1, 'The Oculus - Cache of Eregos (normal)'),
(1, 193603, 1, 'The Oculus - Cache of Eregos (heroic)'),
(1, 190663, 1, 'The Culling of Stratholme - Dark Runed Chest (normal)'),
(1, 193597, 1, 'The Culling of Stratholme - Dark Runed Chest (heroic)'),
(1, 195709, 1, 'Trial of the Champion - Champion''s Cache'),
(1, 195710, 1, 'Trial of the Champion - Champion''s Cache'),
(1, 195374, 1, 'Trial of the Champion - Eadric''s Cache'),
(1, 195375, 1, 'Trial of the Champion - Eadric''s Cache'),
(1, 195323, 1, 'Trial of the Champion - Confessor''s Cache'),
(1, 195324, 1, 'Trial of the Champion - Confessor''s Cache'),
(1, 201710, 1, 'Halls of Reflection - The Captain''s Chest (normal)'),
(1, 202336, 1, 'Halls of Reflection - The Captain''s Chest (heroic)'),
(1, 202212, 1, 'Halls of Reflection - The Captain''s Chest'),
(1, 202337, 1, 'Halls of Reflection - The Captain''s Chest'),

-- Wrath of the Lich King raids
(1, 181366, 1, 'Naxxramas - Four Horsemen Chest (10)'),
(1, 193426, 1, 'Naxxramas - Four Horsemen Chest (25)'),
(1, 193905, 1, 'The Eye of Eternity - Alexstrasza''s Gift (Malygos)'),
(1, 193967, 1, 'The Eye of Eternity - Alexstrasza''s Gift (Malygos)'),
(1, 195046, 1, 'Ulduar - Cache of Living Stone (Kologarn, 10)'),
(1, 195047, 1, 'Ulduar - Cache of Living Stone (Kologarn, 25)'),
(1, 194307, 1, 'Ulduar - Cache of Winter (Hodir, 10)'),
(1, 194308, 1, 'Ulduar - Cache of Winter (Hodir, 25)'),
(1, 194200, 1, 'Ulduar - Rare Cache of Winter (Hodir hard mode, 10)'),
(1, 194201, 1, 'Ulduar - Rare Cache of Winter (Hodir hard mode, 25)'),
(1, 194312, 1, 'Ulduar - Cache of Storms (Thorim, 10)'),
(1, 194314, 1, 'Ulduar - Cache of Storms (Thorim, 25)'),
(1, 194313, 1, 'Ulduar - Cache of Storms (Thorim)'),
(1, 194315, 1, 'Ulduar - Cache of Storms (Thorim)'),
(1, 194789, 1, 'Ulduar - Cache of Innovation (Mimiron, 10)'),
(1, 194956, 1, 'Ulduar - Cache of Innovation (Mimiron, 25)'),
(1, 194957, 1, 'Ulduar - Cache of Innovation (Mimiron hard mode, 10)'),
(1, 194958, 1, 'Ulduar - Cache of Innovation (Mimiron hard mode, 25)'),
(1, 194324, 1, 'Ulduar - Freya''s Gift'),
(1, 194325, 1, 'Ulduar - Freya''s Gift'),
(1, 194326, 1, 'Ulduar - Freya''s Gift'),
(1, 194327, 1, 'Ulduar - Freya''s Gift'),
(1, 194328, 1, 'Ulduar - Freya''s Gift'),
(1, 194329, 1, 'Ulduar - Freya''s Gift'),
(1, 194330, 1, 'Ulduar - Freya''s Gift'),
(1, 194331, 1, 'Ulduar - Freya''s Gift'),
(1, 194821, 1, 'Ulduar - Gift of the Observer (Algalon, 10)'),
(1, 194822, 1, 'Ulduar - Gift of the Observer (Algalon, 25)'),
(1, 195631, 1, 'Trial of the Crusader - Crusaders'' Cache (Faction Champions, 10)'),
(1, 195632, 1, 'Trial of the Crusader - Crusaders'' Cache (Faction Champions, 25)'),
(1, 195633, 1, 'Trial of the Crusader - Crusaders'' Cache (Faction Champions, 10 heroic)'),
(1, 195635, 1, 'Trial of the Crusader - Crusaders'' Cache (Faction Champions, 25 heroic)'),
(1, 195665, 1, 'Trial of the Crusader - Argent Crusade Tribute Chest (10 heroic)'),
(1, 195666, 1, 'Trial of the Crusader - Argent Crusade Tribute Chest (10 heroic)'),
(1, 195667, 1, 'Trial of the Crusader - Argent Crusade Tribute Chest (10 heroic)'),
(1, 195668, 1, 'Trial of the Crusader - Argent Crusade Tribute Chest (10 heroic)'),
(1, 195669, 1, 'Trial of the Crusader - Argent Crusade Tribute Chest (25 heroic)'),
(1, 195670, 1, 'Trial of the Crusader - Argent Crusade Tribute Chest (25 heroic)'),
(1, 195671, 1, 'Trial of the Crusader - Argent Crusade Tribute Chest (25 heroic)'),
(1, 195672, 1, 'Trial of the Crusader - Argent Crusade Tribute Chest (25 heroic)'),
(1, 201872, 1, 'Icecrown Citadel - Gunship Armory (Alliance, 10 heroic)'),
(1, 201873, 1, 'Icecrown Citadel - Gunship Armory (Alliance, 10)'),
(1, 201874, 1, 'Icecrown Citadel - Gunship Armory (Alliance, 25)'),
(1, 201875, 1, 'Icecrown Citadel - Gunship Armory (Alliance, 25 heroic)'),
(1, 202177, 1, 'Icecrown Citadel - Gunship Armory (Horde, 10 heroic)'),
(1, 202178, 1, 'Icecrown Citadel - Gunship Armory (Horde, 10)'),
(1, 202179, 1, 'Icecrown Citadel - Gunship Armory (Horde, 25 heroic)'),
(1, 202180, 1, 'Icecrown Citadel - Gunship Armory (Horde, 25)'),
(1, 202239, 1, 'Icecrown Citadel - Deathbringer''s Cache (10)'),
(1, 202238, 1, 'Icecrown Citadel - Deathbringer''s Cache (10 heroic)'),
(1, 202240, 1, 'Icecrown Citadel - Deathbringer''s Cache (25)'),
(1, 202241, 1, 'Icecrown Citadel - Deathbringer''s Cache (25 heroic)'),
(1, 201959, 1, 'Icecrown Citadel - Cache of the Dreamwalker (10)'),
(1, 202338, 1, 'Icecrown Citadel - Cache of the Dreamwalker (10 heroic)'),
(1, 202339, 1, 'Icecrown Citadel - Cache of the Dreamwalker (25)'),
(1, 202340, 1, 'Icecrown Citadel - Cache of the Dreamwalker (25 heroic)');
