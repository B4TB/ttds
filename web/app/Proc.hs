module Proc (Proc, launch, call, kill, Command, CommandComponent, mkComp, mkCommand, mkUnvalidatedCommand) where

import Control.Concurrent.MVar (MVar, newMVar, withMVar)
import Control.Exception (Exception)
import Control.Exception.Base (throwIO)
import System.IO (BufferMode (..), Handle, hGetLine, hPutStrLn, hSetBuffering)
import System.Process (terminateProcess, CmdSpec (ShellCommand), CreateProcess (..), ProcessHandle, StdStream (CreatePipe, Inherit), createProcess)

type Proc = MVar Process

data Process = Process
  { getStdin :: Handle,
    getStdout :: Handle,
    getHandle :: ProcessHandle
  }

data ProcFailureException = CantGatherProc | NeedCmd deriving (Show)

instance Exception ProcFailureException

newtype Command = Command String

newtype CommandComponent = CommandComponent String
type Target = CommandComponent
type Argument = CommandComponent
type Action = CommandComponent

mkComp :: String -> CommandComponent
mkComp = CommandComponent . validate
  where
    validate (x:xs) = if shouldFilter x then '_' : validate xs else x : validate xs
    validate [] = []

    shouldFilter c = (c == ' ') || (c == ';') || (c == ':') || (c == '\n') || (c == '\0')

mkCommand :: Target -> Action -> [Argument] -> Command
mkCommand target action args = Command $ unComp target ++ ": " ++ unComp action ++ " " ++ (unwords $ map unComp args)
  where
    unComp (CommandComponent s) = s

kill :: Proc -> IO ()
kill lock = withMVar lock $ terminateProcess . getHandle

-- Create a command without validating it and without stripping potentially
-- dangerous characters.
mkUnvalidatedCommand :: String -> Command
mkUnvalidatedCommand = Command

call :: Proc -> Command -> IO String
call lock (Command line) =
  withMVar lock $ \proc ->
    let stdin = getStdin proc
        stdout = getStdout proc
     in hPutStrLn stdin line >> hGetLine stdout

launch :: [String] -> IO Proc
launch [] = throwIO NeedCmd
launch cmd =
  let proc =
        CreateProcess
          { cmdspec = ShellCommand $ unwords cmd,
            std_in = CreatePipe,
            std_err = Inherit,
            std_out = CreatePipe,
            cwd = Nothing,
            env = Nothing,
            close_fds = True,
            create_group = False,
            delegate_ctlc = False,
            detach_console = True,
            create_new_console = False,
            new_session = True,
            child_group = Nothing,
            child_user = Nothing,
            use_process_jobs = False
          }
   in createProcess proc >>= gatherProc >>= newMVar

gatherProc :: (Maybe Handle, Maybe Handle, Maybe Handle, ProcessHandle) -> IO Process
gatherProc (Just stdin, Just stdout, _, handle) =
  hSetBuffering stdout NoBuffering
    >> hSetBuffering stdin NoBuffering
    >> return
      Process
        { getStdin = stdin,
          getStdout = stdout,
          getHandle = handle
        }
gatherProc _ = throwIO CantGatherProc
