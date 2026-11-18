from pathlib import Path
import os, sys, subprocess
sys.path.insert(0,str(Path("tools").resolve()))
import mkfs, test_lab4 as q
d=Path("build/independent-window-audit")
(d/"rules.mk").write_text("$(BUILD)/apps/waudit.elf: build/independent-window-audit/waudit.c $(APP_DEPS)\n\t$(compile_user_program)\n")
paths=[(name,d/"apps"/(name+".bin")) for name in ("sh","waudit")]
subprocess.run(["make","-s","-j4","-f","Makefile","-f",str(d/"rules.mk"),"BUILD="+str(d),"LIFECYCLE=check",q.CONFIG,"build",*(str(path) for _,path in paths)],check=True)
img=(d/"disk.img").resolve()
mkfs.make_image(img,programs=paths)
os.environ.update(MAKEFLAGS=f"BUILD={d} LIFECYCLE=check", DISK=str(img))
q.OUT=d
vm=q.Machine("page-pool")
try:
    vm.until(b"seele$ ")
    for policy in (1,2):
        vm.send(f"waudit {policy}\r".encode())
        response=vm.until(b"seele$ ",timeout=60).decode()
        print(response)
        assert "INDEPENDENT WINDOW AUDIT PASS" in response and "USER FAULT" not in response
finally:
    vm.close()
