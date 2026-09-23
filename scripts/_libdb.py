import os
import sqlite3

db = r"/mnt/c/Users/ton_o/AppData/Local/Vapor/library.db"
c = sqlite3.connect(db)
print("tables", [r[0] for r in c.execute("select name from sqlite_master where type='table'")])
for t in c.execute("select name from sqlite_master where type='table'"):
    print("====", t[0])
    cols = [x[1] for x in c.execute("pragma table_info(%s)" % t[0])]
    print("cols", cols)
    for row in c.execute("select * from %s" % t[0]):
        print(row)

p = "/mnt/c/Users/ton_o/AppData/Local/Vapor/Games"
print("Games", os.listdir(p))
vp = os.path.join(p, ".vapor")
if os.path.isdir(vp):
    print(".vapor", os.listdir(vp))
